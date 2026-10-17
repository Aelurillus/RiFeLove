"""Exercise the native FFmpeg library engine with locally generated fixtures."""

from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLI, FFMPEG, FFPROBE = (Path(argument).resolve() for argument in sys.argv[1:4])
del sys.argv[1:4]


def invoke(executable, *arguments, expected=0):
    result = subprocess.run([str(executable), *(str(a) for a in arguments)],
                            capture_output=True, encoding="utf-8", timeout=30)
    if result.returncode != expected:
        raise AssertionError(f"{executable.name} returned {result.returncode}: {result.stderr}")
    return result


def reference(path, *arguments):
    return json.loads(invoke(FFPROBE, "-v", "error", *arguments, "-of", "json", path).stdout)


class NativeMediaIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="media-fixtures-", dir=CLI.parent)
        cls.root = Path(cls.temporary.name)
        cls.subtitle = cls.root / "captions.srt"
        cls.subtitle.write_text("1\n00:00:00,000 --> 00:00:00,400\nRifeLove synthetic fixture\n", encoding="utf-8")
        cls.chapters = cls.root / "chapters.txt"
        cls.chapters.write_text(";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=500\ntitle=Opening\n", encoding="utf-8")
        cls.source = cls.root / "中文 源视频.mkv"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=24000/1001:duration=0.5",
               "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.5",
               "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=0.5",
               "-i", cls.subtitle, "-f", "ffmetadata", "-i", cls.chapters,
               "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:s", "-map_chapters", "4",
               "-vf", "format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=limited",
               "-c:v", "ffv1", "-level", "3",
               "-c:a", "aac", "-c:s", "srt", "-metadata", "title=RifeLove test",
               "-metadata:s:a:0", "language=zho", "-metadata:s:a:1", "language=jpn",
               "-metadata:s:s:0", "language=eng", cls.source)
        cls.reordered = cls.root / "reordered.mp4"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=24:duration=0.5",
               "-f", "lavfi", "-i", "sine=frequency=440:duration=0.5", "-c:v", "mpeg4", "-bf", "2", "-q:v", "4", "-c:a", "aac", cls.reordered)
        cls.vfr = cls.root / "vfr.mkv"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=30:duration=0.4",
               "-vf", "setpts='if(lt(N,6),N,6+(N-6)*2)/(30*TB)'", "-fps_mode", "vfr", "-c:v", "ffv1", cls.vfr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def probe(self, path):
        return json.loads(invoke(CLI, "media-probe", path).stdout)

    def scan(self, path):
        return [json.loads(line) for line in invoke(CLI, "media-scan", path, "--frames").stdout.splitlines()]

    def test_probe_reports_tracks_hdr_and_chapters(self):
        info = self.probe(self.source)
        self.assertEqual([s["type"] for s in info["streams"]], ["video", "audio", "audio", "subtitle"])
        video = info["streams"][0]
        self.assertEqual((video["width"], video["height"]), (96, 64))
        self.assertEqual(Fraction(video["average_rate"]), Fraction(24000, 1001))
        self.assertEqual(video["color"]["bit_depth"], 10)
        self.assertEqual(video["color"]["transfer"], "smpte2084")
        self.assertTrue(video["color"]["hdr_transfer"])
        self.assertEqual(info["streams"][1]["metadata"]["language"], "zho")
        self.assertEqual(info["streams"][2]["metadata"]["language"], "jpn")
        self.assertEqual(len(info["chapters"]), 1)
        self.assertEqual(info["chapters"][0]["metadata"]["title"], "Opening")

    def test_fractional_cfr_is_not_confused_with_timestamp_quantization(self):
        records = self.scan(self.source)
        self.assertEqual(records[-1]["frames"], 12)
        self.assertEqual(records[-1]["cadence"], "constant")
        self.assertEqual(records[-1]["non_increasing_timestamps"], 0)
        self.assertTrue(all(frame["color"]["bit_depth"] == 10 for frame in records[:-1]))
        self.assertTrue(all(frame["color"]["hdr_transfer"] for frame in records[:-1]))

    def test_b_frame_decoder_drains_and_matches_reference_pts(self):
        records = self.scan(self.reordered)
        native_pts = [Fraction(frame["pts"]) for frame in records[:-1]]
        info = reference(self.reordered, "-show_streams")
        base = Fraction(info["streams"][0]["time_base"])
        expected = reference(self.reordered, "-select_streams", "v:0", "-show_frames", "-show_entries", "frame=best_effort_timestamp")
        expected_pts = [int(frame["best_effort_timestamp"]) * base for frame in expected["frames"]]
        self.assertEqual(native_pts, expected_pts)
        self.assertEqual(len(native_pts), 12)
        self.assertEqual(native_pts, sorted(native_pts))

    def test_vfr_scan_preserves_actual_intervals(self):
        records = self.scan(self.vfr)
        self.assertEqual(records[-1]["cadence"], "variable")
        values = [Fraction(frame["pts"]) for frame in records[:-1]]
        self.assertEqual(len(values), 12)
        self.assertGreater(max(b - a for a, b in zip(values, values[1:])), Fraction(1, 20))

    def test_remux_preserves_packets_tracks_and_metadata(self):
        output = self.root / "复制 输出.mkv"
        original_hash = hashlib.sha256(self.source.read_bytes()).digest()
        result = json.loads(invoke(CLI, "remux", self.source, output).stdout)
        self.assertEqual(result["copied_streams"], 4)
        self.assertGreater(result["packets_written"], 12)
        before = reference(self.source, "-show_packets", "-show_data_hash", "sha256", "-show_entries", "packet=stream_index,pts,dts,duration,data_hash")
        after = reference(output, "-show_packets", "-show_data_hash", "sha256", "-show_entries", "packet=stream_index,pts,dts,duration,data_hash")
        self.assertEqual(before["packets"], after["packets"])
        probed = self.probe(output)
        self.assertEqual(len(probed["streams"]), 4)
        self.assertEqual(probed["metadata"]["title"], "RifeLove test")
        self.assertEqual(probed["streams"][1]["metadata"]["language"], "zho")
        self.assertTrue(probed["streams"][0]["color"]["hdr_transfer"])
        self.assertEqual(len(probed["chapters"]), 1)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).digest(), original_hash)

    def test_mp4_and_mov_preserve_video_timing_and_audio(self):
        original = self.scan(self.reordered)
        for extension in ("mp4", "mov"):
            with self.subTest(extension=extension):
                output = self.root / f"round-trip.{extension}"
                invoke(CLI, "remux", self.reordered, output)
                actual = self.scan(output)
                self.assertEqual([Fraction(f["pts"]) for f in actual[:-1]], [Fraction(f["pts"]) for f in original[:-1]])
                self.assertEqual([s["type"] for s in self.probe(output)["streams"]], ["video", "audio"])
                expected_audio = reference(self.reordered, "-select_streams", "a:0", "-show_packets", "-show_entries", "packet=pts_time,dts_time,duration_time")
                actual_audio = reference(output, "-select_streams", "a:0", "-show_packets", "-show_entries", "packet=pts_time,dts_time,duration_time")
                self.assertEqual(expected_audio["packets"], actual_audio["packets"])

    def test_incompatible_container_fails_without_partial_output(self):
        output = self.root / "incompatible.mp4"
        result = invoke(CLI, "remux", self.source, output, expected=2)
        self.assertIn("support", result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob("*.rifelove-*.partial")), [])

    def test_existing_output_is_never_replaced(self):
        output = self.root / "existing.mkv"
        output.write_bytes(b"preserve existing output")
        invoke(CLI, "remux", self.source, output, expected=2)
        self.assertEqual(output.read_bytes(), b"preserve existing output")

    def test_corrupt_and_network_inputs_are_reported(self):
        corrupt = self.root / "corrupt.mp4"
        corrupt.write_bytes(b"not a video")
        self.assertEqual(invoke(CLI, "media-probe", corrupt, expected=2).stdout, "")
        result = invoke(CLI, "media-probe", "https://example.invalid/video.mp4", expected=2)
        self.assertIn("local regular file", result.stderr)


if __name__ == "__main__":
    unittest.main()

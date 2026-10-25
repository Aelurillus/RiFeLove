"""Exercise the native FFmpeg library engine with locally generated fixtures."""

from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLI, FFMPEG, FFPROBE, METADATA_FIXTURE = (Path(argument).resolve() for argument in sys.argv[1:5])
del sys.argv[1:5]


def invoke(executable, *arguments, expected=0):
    result = subprocess.run([str(executable), *(str(a) for a in arguments)],
                            capture_output=True, encoding="utf-8", timeout=30)
    if result.returncode != expected:
        raise AssertionError(f"{executable.name} returned {result.returncode}: {result.stderr}")
    return result


def reference(path, *arguments):
    return json.loads(invoke(FFPROBE, "-v", "error", *arguments, "-of", "json", path).stdout)


def semantic_side_data(side):
    result = {}
    for key, value in side.items():
        if key != "side_data_type" and isinstance(value, (str, int, float)) and not isinstance(value, bool):
            try:
                value = Fraction(str(value))
            except (ValueError, ZeroDivisionError):
                pass
        result[key] = value
    return result


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
        cls.rotated = cls.root / "rotated.mp4"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-display_rotation", "90", "-i", cls.reordered, "-c", "copy", cls.rotated)
        cls.horizontal_flip_rotated = cls.root / "horizontal-flip-rotated.mp4"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-display_hflip", "-display_rotation", "90",
               "-i", cls.reordered, "-c", "copy", cls.horizontal_flip_rotated)
        cls.vertical_flip_rotated = cls.root / "vertical-flip-rotated.mp4"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-display_vflip", "-display_rotation", "270",
               "-i", cls.reordered, "-c", "copy", cls.vertical_flip_rotated)
        cls.hdr_metadata_base = cls.root / "hdr-metadata-base.mkv"
        invoke(FFMPEG, "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=24:duration=0.5",
               "-vf", "format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=limited",
               "-c:v", "ffv1", "-level", "3", cls.hdr_metadata_base)
        cls.hdr_metadata_rotated = cls.root / "hdr-metadata-rotated.mkv"
        invoke(METADATA_FIXTURE, cls.hdr_metadata_base, cls.hdr_metadata_rotated)
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

    def test_media_capabilities_open_encoder_contexts_for_source_profile(self):
        report = json.loads(invoke(CLI, "media-capabilities", self.reordered, "--fps", "60000/1001").stdout)
        self.assertEqual(report["event"], "media_capabilities")
        self.assertEqual((report["width"], report["height"], report["pixel_format"]), (96, 64, "yuv420p"))
        self.assertEqual(report["probe_fps"], "60000/1001")
        self.assertEqual(report["probe_scope"], "encoder_context_open_only")
        self.assertFalse(report["frames_encoded"])
        encoders = {entry["name"]: entry for entry in report["encoders"]}
        self.assertEqual(set(encoders), {"h264_nvenc", "libopenh264", "libkvazaar", "libaom-av1"})
        for entry in encoders.values():
            self.assertIsInstance(entry["registered"], bool)
            self.assertIsInstance(entry["context_opened"], bool)
            self.assertEqual(entry["context_opened"], entry["registered"] and not bool(entry["reason"]))
            if entry["registered"] and not entry["context_opened"]:
                self.assertTrue(entry["reason"])

    def test_display_rotation_matches_ffprobe_and_survives_remux_and_transcode(self):
        source = self.probe(self.rotated)["streams"][0]
        reference_stream = reference(self.rotated, "-show_streams")["streams"][0]
        reference_rotation = next(side["rotation"] for side in reference_stream["side_data_list"]
                                  if side["side_data_type"] == "Display Matrix")
        self.assertAlmostEqual(source["rotation_degrees"], reference_rotation, delta=0.01)
        for extension in ("mp4", "mov", "mkv"):
            for operation in ("remux", "transcode"):
                with self.subTest(operation=operation, extension=extension):
                    output = self.root / f"rotation-{operation}.{extension}"
                    if operation == "remux":
                        invoke(CLI, "remux", self.rotated, output)
                    else:
                        invoke(CLI, "transcode", self.rotated, output, "--encoder", "libopenh264")
                    output_stream = self.probe(output)["streams"][0]
                    self.assertAlmostEqual(output_stream["rotation_degrees"], source["rotation_degrees"], delta=0.01)
                    self.assertTrue(any(side["name"] == "Display Matrix" for side in output_stream["side_data"]))

    def test_display_matrix_preserves_horizontal_and_vertical_reflections(self):
        fixtures = (("horizontal", self.horizontal_flip_rotated), ("vertical", self.vertical_flip_rotated))
        for label, path in fixtures:
            with self.subTest(reflection=label):
                source = self.probe(path)["streams"][0]
                source_reference = reference(path, "-show_streams")["streams"][0]
                reference_data = next(side for side in source_reference["side_data_list"]
                                      if side["side_data_type"] == "Display Matrix")
                reference_text = reference_data["displaymatrix"]
                reference_matrix = [int(value) for line in reference_text.strip().splitlines()
                                    for value in line.split(":", 1)[1].split()]
                self.assertEqual(source["display_matrix"], reference_matrix)
                self.assertAlmostEqual(source["rotation_degrees"], reference_data["rotation"], delta=0.01)
                self.assertEqual(len(source["display_matrix"]), 9)
                for extension in ("mp4", "mov", "mkv"):
                    for operation in ("remux", "transcode"):
                        with self.subTest(operation=operation, extension=extension):
                            output = self.root / f"display-matrix-{label}-{operation}.{extension}"
                            if operation == "remux":
                                invoke(CLI, "remux", path, output)
                            else:
                                invoke(CLI, "transcode", path, output, "--encoder", "libopenh264")
                            result = self.probe(output)["streams"][0]
                            self.assertEqual(result["display_matrix"], source["display_matrix"])
                            self.assertAlmostEqual(result["rotation_degrees"], source["rotation_degrees"], delta=0.01)

    def test_static_hdr_metadata_survives_rotation_and_transcode(self):
        source = self.probe(self.hdr_metadata_rotated)["streams"][0]
        self.assertEqual(source["color"]["mastering_display"], {
            "min_luminance": "1/200", "max_luminance": "1000/1",
            "primaries": [["17/25", "8/25"], ["53/200", "69/100"], ["3/20", "3/50"]],
            "white_point": ["3127/10000", "329/1000"],
        })
        self.assertEqual(source["color"]["content_light"], {"max_cll": 1000, "max_fall": 400})
        self.assertEqual(len(source["display_matrix"]), 9)
        self.assertEqual(source["color"]["bit_depth"], 10)
        self.assertEqual(source["color"]["transfer"], "smpte2084")
        self.assertTrue(source["color"]["hdr_transfer"])
        reference_stream = reference(self.hdr_metadata_rotated, "-show_streams")["streams"][0]
        reference_side_data = {side["side_data_type"]: side for side in reference_stream["side_data_list"]}
        self.assertIn("Mastering display metadata", reference_side_data)
        self.assertIn("Content light level metadata", reference_side_data)
        self.assertIn("Display Matrix", reference_side_data)
        matrix_text = reference_side_data["Display Matrix"]["displaymatrix"]
        reference_matrix = [int(value) for line in matrix_text.strip().splitlines()
                            for value in line.split(":", 1)[1].split()]
        self.assertEqual(source["display_matrix"], reference_matrix)
        self.assertAlmostEqual(source["rotation_degrees"], reference_side_data["Display Matrix"]["rotation"], delta=0.01)

        remuxed = self.root / "hdr-metadata-remuxed.mkv"
        invoke(CLI, "remux", self.hdr_metadata_rotated, remuxed)
        transcoded_outputs = [remuxed]
        for extension in ("mp4", "mkv"):
            output = self.root / f"hdr-metadata-transcoded.{extension}"
            invoke(CLI, "transcode", self.hdr_metadata_rotated, output, "--encoder", "libaom-av1",
                   "--option", "cpu-used=8", "--option", "threads=2")
            transcoded_outputs.append(output)

        for output in transcoded_outputs:
            with self.subTest(output=output.suffix):
                result = self.probe(output)["streams"][0]
                self.assertEqual(result["color"]["mastering_display"], source["color"]["mastering_display"])
                self.assertEqual(result["color"]["content_light"], source["color"]["content_light"])
                self.assertEqual(result["display_matrix"], source["display_matrix"])
                output_reference = reference(output, "-show_streams")["streams"][0]
                output_side_data = {side["side_data_type"]: side for side in output_reference["side_data_list"]}
                for side_type in ("Mastering display metadata", "Content light level metadata", "Display Matrix"):
                    self.assertIn(side_type, output_side_data)
                    self.assertEqual(semantic_side_data(output_side_data[side_type]),
                                     semantic_side_data(reference_side_data[side_type]))

    def test_fractional_cfr_is_not_confused_with_timestamp_quantization(self):
        records = self.scan(self.source)
        self.assertEqual(records[-1]["frames"], 12)
        self.assertEqual(records[-1]["cadence"], "constant")
        self.assertEqual(records[-1]["non_increasing_timestamps"], 0)
        self.assertEqual(records[-1]["recovered_timestamps"], 0)
        self.assertTrue(all(not frame["pts_recovered"] for frame in records[:-1]))
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
        self.assertEqual(records[-1]["recovered_timestamps"], 0)
        values = [Fraction(frame["pts"]) for frame in records[:-1]]
        self.assertTrue(all(not frame["pts_recovered"] for frame in records[:-1]))
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

    def test_lossless_transcode_preserves_hdr_pixels_auxiliary_packets_and_timing(self):
        output = self.root / "重新编码 HDR.mkv"
        records = [json.loads(line) for line in invoke(CLI, "transcode", self.source, output, "--encoder", "ffv1", "--progress").stdout.splitlines()]
        self.assertEqual(records[-1]["event"], "transcode_completed")
        self.assertEqual(records[-1]["frames_submitted"], 12)
        self.assertGreater(records[-1]["elapsed_seconds"], 0)
        self.assertGreater(records[-1]["processing_fps"], 0)
        self.assertEqual(len([r for r in records if r["event"] == "encode_progress"]), 12)
        self.assertEqual([r["pts"] for r in self.scan(self.source)[:-1]], [r["pts"] for r in self.scan(output)[:-1]])
        info = self.probe(output)
        self.assertEqual(len(info["streams"]), 4)
        self.assertTrue(info["streams"][0]["color"]["hdr_transfer"])
        self.assertEqual(info["streams"][0]["color"]["bit_depth"], 10)
        self.assertEqual(info["streams"][1]["metadata"]["language"], "zho")
        self.assertEqual(info["chapters"][0]["metadata"]["title"], "Opening")
        def pixels(path):
            text = invoke(FFMPEG, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-f", "framemd5", "-").stdout
            return [line.rsplit(",", 1)[-1].strip() for line in text.splitlines() if line and not line.startswith("#")]
        self.assertEqual(pixels(self.source), pixels(output))
        for selector in ("a", "s"):
            before = reference(self.source, "-select_streams", selector, "-show_packets", "-show_data_hash", "sha256", "-show_entries", "packet=stream_index,pts,dts,duration,data_hash")
            after = reference(output, "-select_streams", selector, "-show_packets", "-show_data_hash", "sha256", "-show_entries", "packet=stream_index,pts,dts,duration,data_hash")
            self.assertEqual(before["packets"], after["packets"])

    def test_h264_hevc_and_av1_software_encoding(self):
        for encoder, codec, options in (
            ("libopenh264", "h264", []),
            ("libkvazaar", "hevc", []),
            ("libaom-av1", "av1", ["--option", "cpu-used=8", "--option", "threads=2"]),
        ):
            with self.subTest(encoder=encoder):
                output = self.root / f"{codec}-encoded.mp4"
                result = json.loads(invoke(CLI, "transcode", self.reordered, output, "--encoder", encoder, *options).stdout)
                self.assertEqual(result["frames_submitted"], 12)
                self.assertEqual(self.probe(output)["streams"][0]["codec"], codec)
                self.assertEqual([r["pts"] for r in self.scan(self.reordered)[:-1]], [r["pts"] for r in self.scan(output)[:-1]])
                expected_audio = reference(self.reordered, "-select_streams", "a:0", "-show_packets", "-show_entries", "packet=pts_time,dts_time,duration_time")
                actual_audio = reference(output, "-select_streams", "a:0", "-show_packets", "-show_entries", "packet=pts_time,dts_time,duration_time")
                self.assertEqual(expected_audio["packets"], actual_audio["packets"])

    def test_encoder_b_frame_drain_and_vfr(self):
        output = self.root / "b-frame-encoded.mp4"
        invoke(CLI, "transcode", self.reordered, output, "--encoder", "mpeg4", "--bframes", "2")
        self.assertEqual([r["pts"] for r in self.scan(self.reordered)[:-1]], [r["pts"] for r in self.scan(output)[:-1]])
        output_vfr = self.root / "vfr-encoded.mkv"
        invoke(CLI, "transcode", self.vfr, output_vfr, "--encoder", "ffv1")
        before, after = self.scan(self.vfr), self.scan(output_vfr)
        self.assertEqual([r["pts"] for r in before[:-1]], [r["pts"] for r in after[:-1]])
        self.assertEqual(after[-1]["cadence"], "variable")

    def test_invalid_encoder_and_hdr_precision_reduction_do_not_publish(self):
        for name, extra in (
            ("missing-encoder", ["--encoder", "does-not-exist"]),
            ("hdr-reduction", ["--encoder", "libopenh264", "--pixel-format", "yuv420p"]),
            ("hdr-reduction-override", ["--encoder", "libopenh264", "--pixel-format", "yuv420p", "--allow-precision-loss"]),
            ("bad-option", ["--encoder", "ffv1", "--option", "no-such-option=1"]),
        ):
            with self.subTest(name=name):
                output = self.root / f"{name}.mkv"
                invoke(CLI, "transcode", self.source, output, *extra, expected=2)
                self.assertFalse(output.exists())
                self.assertEqual(list(self.root.glob("*.rifelove-*.partial")), [])

    def test_av1_encoding_retains_ten_bit_hdr_and_timing(self):
        output = self.root / "hdr-av1.mkv"
        invoke(CLI, "transcode", self.source, output, "--encoder", "libaom-av1", "--bitrate", "0", "--option", "crf=40", "--option", "cpu-used=8", "--option", "threads=2")
        video = self.probe(output)["streams"][0]
        self.assertEqual(video["codec"], "av1")
        self.assertEqual(video["color"]["bit_depth"], 10)
        self.assertTrue(video["color"]["hdr_transfer"])
        self.assertEqual([f["pts"] for f in self.scan(self.source)[:-1]], [f["pts"] for f in self.scan(output)[:-1]])

    def test_transcode_preserves_existing_output(self):
        output = self.root / "existing-transcode.mp4"
        output.write_bytes(b"existing file")
        invoke(CLI, "transcode", self.reordered, output, "--encoder", "libopenh264", expected=2)
        self.assertEqual(output.read_bytes(), b"existing file")


if __name__ == "__main__":
    unittest.main()

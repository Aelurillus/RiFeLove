"""Exercise a local QA desktop package with VFR, audio, subtitles and chapters.

This developer test creates only synthetic inputs in a new verification folder.
It does not download models, touch user videos, or clean existing artifacts.
"""
from __future__ import annotations

import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def plain_directory(path: Path, root: Path) -> Path:
    """Reject paths outside the test root and existing junctions/symlinks."""
    path = Path(os.path.abspath(path))
    root = Path(os.path.abspath(root))
    if not root.is_relative_to(ROOT) or not path.is_relative_to(root):
        raise ValueError(f"Test path is outside its designated root: {path}")
    entry = ROOT
    for component in path.relative_to(ROOT).parts:
        entry = entry / component
        try:
            attributes = getattr(entry.lstat(), "st_file_attributes", 0)
        except FileNotFoundError:
            continue
        if attributes & 0x400 or entry.is_symlink():
            raise ValueError(f"Test path contains a link or junction: {entry}")
    return path


def probe(ffprobe: Path, path: Path, *options: str) -> dict:
    command = [str(ffprobe), "-v", "error", *options, "-of", "json", str(path)]
    return json.loads(subprocess.check_output(command))


def timestamp(value: str) -> Fraction:
    hours, minutes, seconds = value.split(":")
    return 3600 * int(hours) + 60 * int(minutes) + Fraction(seconds)


def packets(ffprobe: Path, path: Path, stream: str) -> list[dict]:
    return probe(ffprobe, path, "-select_streams", stream, "-show_packets", "-show_data_hash", "sha256",
                 "-show_entries", "packet=pts_time,duration_time,data_hash")["packets"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--ffprobe", required=True, type=Path)
    parser.add_argument("--independent-ffmpeg", required=True, type=Path)
    parser.add_argument("--label", required=True, help="New evidence directory name, not an existing path")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,79}", args.label):
        raise ValueError("Evidence label must be a simple new directory name")
    package = plain_directory(args.package, ROOT / "out")
    if not package.name.endswith("-qa"):
        raise ValueError("Use a dedicated QA package whose directory ends in -qa")
    if (package / "portable-mode.txt").read_bytes() != b"RifeLovePortable 1\n":
        raise ValueError("QA package must have its permanent portable marker")
    manifest = json.loads((package / "PACKAGE-MANIFEST.json").read_text(encoding="utf-8-sig"))
    if manifest.get("portableModeEnabled") is not True or manifest.get("dataDirectory") != "data":
        raise ValueError("QA package must use an independent portable data directory")
    data = plain_directory(package / "data", package)
    if not data.is_dir():
        raise ValueError("QA package data directory is missing")
    evidence = plain_directory(ROOT / "work" / "verification" / args.label, ROOT / "work" / "verification")
    evidence.mkdir(parents=False, exist_ok=False)
    source, output = evidence / "source.mkv", evidence / "output.mkv"
    captions = evidence / "captions.srt"
    chapters = evidence / "chapters.ffmeta"
    captions.write_text("1\n00:00:00,200 --> 00:00:01,500\nFirst scene\n\n"
                        "2\n00:00:02,100 --> 00:00:03,800\nSecond scene\n", encoding="utf-8")
    chapters.write_text(";FFMETADATA1\ntitle=RifeLove synthetic VFR test\n"
                        "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=First\n"
                        "[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=4000\ntitle=Second\n", encoding="utf-8")
    ffmpeg, ffprobe, independent, driver = (p.resolve(strict=True) for p in
                                            (args.ffmpeg, args.ffprobe, args.independent_ffmpeg, args.driver))
    build_source = [str(ffmpeg), "-v", "error", "-n", "-f", "lavfi", "-i",
                    "testsrc2=size=192x128:rate=60:duration=4", "-f", "lavfi", "-i",
                    "sine=frequency=440:sample_rate=48000:duration=4", "-f", "lavfi", "-i",
                    "sine=frequency=880:sample_rate=48000:duration=4", "-f", "srt", "-i", str(captions),
                    "-f", "ffmetadata", "-i", str(chapters), "-map", "0:v:0", "-map", "1:a:0", "-map",
                    "2:a:0", "-map", "3:s:0", "-map_metadata", "4", "-map_chapters", "4", "-vf",
                    "select='not(mod(n,2))+not(mod(n,5))',setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
                    "-fps_mode:v", "vfr", "-c:v", "ffv1", "-pix_fmt", "yuv420p", "-c:a", "flac", "-c:s", "srt",
                    "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=jpn",
                    "-metadata:s:s:0", "language=chi", "-t", "4", str(source)]
    subprocess.run(build_source, check=True, timeout=60)
    source_info = probe(ffprobe, source, "-count_frames", "-show_streams", "-show_chapters", "-show_format")
    source_frames = probe(ffprobe, source, "-select_streams", "v:0", "-show_frames", "-show_entries",
                          "frame=best_effort_timestamp_time")["frames"]
    source_pts = [Fraction(frame["best_effort_timestamp_time"]) for frame in source_frames]
    intervals = Counter(round(float(b - a) * 1000) for a, b in zip(source_pts, source_pts[1:]))
    if len(intervals) < 2 or len(source_info["streams"]) != 4 or len(source_info["chapters"]) != 2:
        raise AssertionError("Synthetic input did not have VFR, four streams and two chapters")
    video_end = timestamp(source_info["streams"][0]["tags"]["DURATION"])
    expected_frames = math.ceil(video_end * 60)
    if expected_frames <= 120:
        raise AssertionError("Fixture must reach the 120-frame validation boundary")
    with source.open("rb") as stream:
        source_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    process = subprocess.run([str(driver), str(package / "RifeLoveNative.exe"), str(source), str(output)],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
    (evidence / "driver.stdout.txt").write_text(process.stdout, encoding="utf-8")
    (evidence / "driver.stderr.txt").write_text(process.stderr, encoding="utf-8")
    if process.returncode != 0 or not output.is_file():
        raise RuntimeError(f"Packaged desktop did not publish the fixture; inspect {evidence}")
    with source.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != source_hash:
            raise AssertionError("Source fixture was modified during desktop processing")
    database = data / "queue.sqlite"
    if not database.is_file():
        raise AssertionError("The portable QA queue was not created")
    with sqlite3.connect(database.as_uri() + "?mode=ro", uri=True, timeout=3) as connection:
        row = connection.execute(
            "SELECT t.state,d.configuration,d.result FROM tasks t JOIN task_details d ON d.task_id=t.id "
            "WHERE t.source_path=? AND t.output_path=? ORDER BY t.rowid DESC LIMIT 1",
            (str(source), str(output)),
        ).fetchone()
    if not row:
        raise AssertionError("The portable QA queue does not contain this test job")
    configuration, task_result = json.loads(row[1]), json.loads(row[2] or "{}")
    if (row[0] != "completed" or configuration.get("targetRate") != "60/1" or
            task_result.get("encoder") != "h264_nvenc" or task_result.get("inferenceCalls", 0) <= 0 or
            task_result.get("outputFrames") != expected_frames or
            task_result.get("validation", {}).get("decodedFrames") != expected_frames):
        raise AssertionError("QA task did not prove completion, actual NVENC, RIFE inference and validation")
    subprocess.run([str(independent), "-v", "error", "-i", str(output), "-map", "0:v:0", "-map", "0:a:0",
                    "-map", "0:a:1", "-f", "null", "-"], check=True, timeout=60)
    actual = probe(ffprobe, output, "-count_frames", "-show_streams", "-show_chapters", "-show_format")
    frames = probe(ffprobe, output, "-select_streams", "v:0", "-show_frames", "-show_entries",
                   "frame=best_effort_timestamp_time")["frames"]
    pts = [Fraction(frame["best_effort_timestamp_time"]) for frame in frames]
    if len(pts) != expected_frames or not all(a < b for a, b in zip(pts, pts[1:])):
        raise AssertionError("Output frame count or PTS ordering is wrong")
    maximum_error = max(abs(pts[index] - Fraction(index, 60)) for index in range(len(pts)))
    if maximum_error > Fraction(1, 2000):
        raise AssertionError("Output PTS departs from the 60 FPS clock by more than half a Matroska tick")
    streams = actual["streams"]
    if [(s["codec_type"], s["codec_name"]) for s in streams] != [
        ("video", "h264"), ("audio", "flac"), ("audio", "flac"), ("subtitle", "subrip")]:
        raise AssertionError("Output does not contain the expected video, two audio tracks and subtitle")
    if [s.get("tags", {}).get("language") for s in streams[1:]] != ["eng", "jpn", "chi"]:
        raise AssertionError("Track language metadata changed")
    if any(streams[0].get(key) != "bt709" for key in ("color_space", "color_transfer", "color_primaries")):
        raise AssertionError("BT.709 video color tags changed")
    if abs(timestamp(streams[0]["tags"]["DURATION"]) - video_end) > Fraction(1, 1000):
        raise AssertionError("Output video end differs from the source video boundary")
    if abs(Fraction(actual["format"]["duration"]) - Fraction(source_info["format"]["duration"])) > Fraction(1, 1000):
        raise AssertionError("Output container duration differs from the source")
    for stream in ("a:0", "a:1", "s:0"):
        if packets(ffprobe, source, stream) != packets(ffprobe, output, stream):
            raise AssertionError(f"Copied packet timestamps, durations or content differ for {stream}")
    def chapter_contract(items: list[dict]) -> list[tuple]:
        return [(item["start_time"], item["end_time"], item.get("tags", {}).get("title")) for item in items]
    if chapter_contract(source_info["chapters"]) != chapter_contract(actual["chapters"]):
        raise AssertionError("Chapter boundaries or titles changed")
    report = {"result": "passed", "synthetic_only": True, "source_frames": len(source_pts),
              "source_interval_ms": dict(intervals), "target_rate": "60/1", "output_frames": len(pts),
              "maximum_pts_error_ms": float(maximum_error * 1000), "audio_tracks": 2,
              "audio_and_subtitle_packets_identical": True, "chapters_identical": True,
              "queue_state": row[0], "encoder": task_result["encoder"],
              "inference_backend": task_result.get("backend"), "inference_calls": task_result["inferenceCalls"],
              "source_unchanged": True,
              "source_video_end": str(video_end), "container_duration": actual["format"]["duration"],
              "independent_decode_passed": True}
    (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Prepare hash-bound RifeLove media inputs from the licensed Sintel trailer frame manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path
from typing import Any


EXPECTED_ARCHIVE_SHA256 = "6324f7ea1a054d2e0b477c070c8f291b7a796926ba7199af51b1701d80c5911b"
FRAME_COUNT = 1_253
SOURCE_FRAME_COUNT = 627
OUTPUT_FRAME_COUNT = 1_254
VIDEO_WIDTH = 1_280
VIDEO_HEIGHT = 545
SOURCE_RATE = "12/1"
REFERENCE_RATE = "24/1"
COLOR_CONTRACT = {
    "primaries": "bt709",
    "transfer": "bt709",
    "matrix": "bt709",
    "range": "tv",
    "pixelFormat": "yuv444p",
    "sourcePixelInterpretation": "BT.709 RGB values are an explicit benchmark convention; source PNGs have no color-signaling chunks.",
}
RGB_TO_YUV_FILTER = (
    "setparams=range=full:color_primaries=bt709:color_trc=bt709:colorspace=gbr,"
    "scale=out_color_matrix=bt709:out_range=tv"
)
YUV_FORMAT_FILTER = (
    "format=yuv444p,"
    "setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_frame_manifest(manifest: dict[str, Any], frames_directory: Path) -> None:
    records = manifest.get("frames")
    if not isinstance(records, list) or len(records) != FRAME_COUNT:
        raise ValueError(f"manifest must contain exactly {FRAME_COUNT} frame records")

    expected_names = [f"sintel_trailer_2k_{index:04d}.png" for index in range(1, FRAME_COUNT + 1)]
    actual_names = sorted(path.name for path in frames_directory.glob("sintel_trailer_2k_*.png"))
    if actual_names != expected_names:
        raise ValueError("extracted PNG names do not exactly match the numbered manifest sequence")

    for index, (record, expected_name) in enumerate(zip(records, expected_names), start=1):
        if not isinstance(record, dict) or record.get("index") != index or record.get("file") != expected_name:
            raise ValueError(f"manifest frame ordering/name mismatch at index {index}")
        path = frames_directory / expected_name
        if record.get("bytes") != path.stat().st_size:
            raise ValueError(f"frame byte size differs from manifest: {expected_name}")
        if record.get("sha256") != sha256_file(path):
            raise ValueError(f"frame SHA-256 differs from manifest: {expected_name}")
        if index % 250 == 0 or index == FRAME_COUNT:
            print(f"Verified frame hashes: {index}/{FRAME_COUNT}", flush=True)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=14_400)
    if result.returncode:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {command[0]}\n"
            f"stdout:\n{result.stdout[-2000:]}\nstderr:\n{result.stderr[-4000:]}"
        )
    return result


def version(tool: Path) -> str:
    result = run([str(tool), "-version"])
    return result.stdout.splitlines()[0].strip()


def probe_video(ffprobe: Path, path: Path) -> dict[str, Any]:
    result = run([
        str(ffprobe), "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries",
        "stream=width,height,pix_fmt,avg_frame_rate,r_frame_rate,time_base,nb_read_frames,color_range,color_space,color_transfer,color_primaries",
        "-show_entries", "format=duration", "-of", "json", str(path),
    ])
    document = json.loads(result.stdout)
    if len(document.get("streams", [])) != 1:
        raise ValueError(f"expected one video stream in {path.name}")
    stream = document["streams"][0]
    stream["duration"] = document.get("format", {}).get("duration")
    return stream


def verify_video(path: Path, stream: dict[str, Any], *, frames: int, rate: str, duration: Fraction) -> None:
    expected = {
        "width": VIDEO_WIDTH,
        "height": VIDEO_HEIGHT,
        "pix_fmt": "yuv444p",
        "avg_frame_rate": rate,
        "r_frame_rate": rate,
        "color_range": "tv",
        "color_space": "bt709",
        "color_transfer": "bt709",
        "color_primaries": "bt709",
        "nb_read_frames": str(frames),
    }
    for key, value in expected.items():
        if stream.get(key) != value:
            raise ValueError(f"{path.name}: expected {key}={value!r}, got {stream.get(key)!r}")
    actual_duration = Fraction(stream["duration"])
    if abs(actual_duration - duration) > Fraction(1, 1000):
        raise ValueError(f"{path.name}: duration {actual_duration} differs from expected {duration}")


def ffmpeg_command(ffmpeg: Path, pattern: Path, destination: Path, *, mode: str, force: bool) -> list[str]:
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-framerate", "24", "-start_number", "1",
        "-i", str(pattern),
    ]
    crop = "crop=1280:545:0:88"
    if mode == "source":
        filters = f"{crop},{RGB_TO_YUV_FILTER},select='not(mod(n,2))',setpts=N/(12*TB),{YUV_FORMAT_FILTER}"
        command += ["-vf", filters, "-r", "12", "-fps_mode", "cfr"]
    elif mode == "reference":
        filters = f"{crop},{RGB_TO_YUV_FILTER},{YUV_FORMAT_FILTER}"
        command += ["-vf", filters, "-r", "24", "-fps_mode", "cfr"]
    elif mode == "pipeline-reference":
        filters = f"{crop},{RGB_TO_YUV_FILTER},{YUV_FORMAT_FILTER},tpad=stop=1:stop_mode=clone"
        command += ["-vf", filters, "-r", "24", "-fps_mode", "cfr"]
    else:
        raise ValueError(f"unknown media mode: {mode}")
    command += [
        "-map", "0:v:0", "-an", "-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1",
        "-pix_fmt", "yuv444p", "-color_range", "tv", "-f", "matroska",
        "-y" if force else "-n", str(destination),
    ]
    return command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True, help="verified prepare_sintel_manifest.py output")
    parser.add_argument("--frames", type=Path, required=True, help="directory containing the numbered PNG frames")
    parser.add_argument("--output-directory", type=Path, required=True, help="local directory for the prepared videos")
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--ffprobe", type=Path, required=True)
    parser.add_argument("--force", action="store_true", help="replace existing prepared videos and report")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve(strict=True)
    frames_directory = args.frames.resolve(strict=True)
    ffmpeg = args.ffmpeg.resolve(strict=True)
    ffprobe = args.ffprobe.resolve(strict=True)
    output_directory = args.output_directory.resolve()
    if not manifest_path.is_file() or not frames_directory.is_dir():
        raise ValueError("manifest and frame-directory inputs are invalid")
    if not ffmpeg.is_file() or not ffprobe.is_file():
        raise ValueError("FFmpeg and FFprobe must be local executable files")

    source_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if source_manifest.get("datasetId") != "sintel-trailer-720p-xiph-png":
        raise ValueError("manifest is not the verified Xiph 720p Sintel trailer")
    if source_manifest.get("source", {}).get("archiveSha256") != EXPECTED_ARCHIVE_SHA256:
        raise ValueError("manifest archive identity differs from the pinned Xiph asset")
    if source_manifest.get("sequence", {}).get("frameCount") != FRAME_COUNT:
        raise ValueError(f"manifest must contain {FRAME_COUNT} frames")
    verify_frame_manifest(source_manifest, frames_directory)

    pattern = frames_directory / "sintel_trailer_2k_%04d.png"
    output_directory.mkdir(parents=True, exist_ok=True)
    names = {
        "source": "source-12fps.mkv",
        "reference": "reference-24fps-real.mkv",
        "pipelineReference": "reference-24fps-pipeline.mkv",
    }
    final_paths = {key: output_directory / name for key, name in names.items()}
    report_path = output_directory / "prepared-media.json"
    existing = [path for path in [*final_paths.values(), report_path] if path.exists()]
    if existing and not args.force:
        raise FileExistsError(f"prepared output already exists: {existing[0]}; pass --force to replace it")

    output_directory_resolved = output_directory.resolve()
    temporary_paths: dict[str, Path] = {}
    try:
        for key, path in final_paths.items():
            temporary_paths[key] = output_directory_resolved / f".{path.stem}.{os.getpid()}.tmp.mkv"
        for key, mode in (("source", "source"), ("reference", "reference"), ("pipelineReference", "pipeline-reference")):
            print(f"Preparing {key}: {temporary_paths[key].name}", flush=True)
            run(ffmpeg_command(ffmpeg, pattern, temporary_paths[key], mode=mode, force=False))

        expected = {
            "source": (SOURCE_FRAME_COUNT, SOURCE_RATE, Fraction(209, 4)),
            "reference": (FRAME_COUNT, REFERENCE_RATE, Fraction(FRAME_COUNT, 24)),
            "pipelineReference": (OUTPUT_FRAME_COUNT, REFERENCE_RATE, Fraction(209, 4)),
        }
        probed: dict[str, dict[str, Any]] = {}
        for key, path in temporary_paths.items():
            probed[key] = probe_video(ffprobe, path)
            verify_video(path, probed[key], frames=expected[key][0], rate=expected[key][1], duration=expected[key][2])

        ffmpeg_version = version(ffmpeg)
        ffprobe_version = version(ffprobe)
        for key, temporary_path in temporary_paths.items():
            os.replace(temporary_path, final_paths[key])

        prepared = {
            "schemaVersion": 1,
            "datasetId": "sintel-trailer-720p-xiph-png",
            "sourceArchiveSha256": EXPECTED_ARCHIVE_SHA256,
            "frameManifest": manifest_path.name,
            "attribution": "© copyright Blender Foundation | durian.blender.org",
            "activePictureCrop": {"x": 0, "y": 88, "width": VIDEO_WIDTH, "height": VIDEO_HEIGHT,
                                  "verifiedBlackBars": {"topRows": 88, "bottomRows": 87}},
            "evaluationColorContract": COLOR_CONTRACT,
            "ffmpeg": {"path": str(ffmpeg), "version": ffmpeg_version, "sha256": sha256_file(ffmpeg)},
            "ffprobe": {"path": str(ffprobe), "version": ffprobe_version, "sha256": sha256_file(ffprobe)},
            "files": {},
            "terminalHold": {
                "pipelineReferenceFile": names["pipelineReference"],
                "appendedFrames": 1,
                "reason": "Match the 52.25 s 12 fps source extent; the appended final frame is a boundary hold, not a rendered Sintel frame.",
            },
            "limitations": [
                "Sintel is rendered animation and does not represent camera-captured footage.",
                "Source PNGs contain no sRGB, gAMA, cHRM, iCCP, or cICP color-signaling chunks.",
                "BT.709 RGB interpretation is an explicit benchmark convention, not source metadata.",
                "The benchmark is not yet partitioned by reviewed shot boundaries.",
                "Candidate training-set overlap is unknown.",
            ],
        }
        for key, path in final_paths.items():
            prepared["files"][key] = {
                "file": path.name,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
                "probe": probed[key],
            }

        encoded = (json.dumps(prepared, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        with tempfile.NamedTemporaryFile("wb", dir=output_directory_resolved, prefix=".prepared-media.", suffix=".tmp", delete=False) as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
            report_temporary = Path(stream.name)
        try:
            os.replace(report_temporary, report_path)
        finally:
            if report_temporary.exists():
                report_temporary.unlink()
    finally:
        for path in temporary_paths.values():
            if path.exists():
                path.unlink()

    print(f"prepared_media={report_path}")
    for key, value in prepared["files"].items():
        print(f"{key}_frames={value['probe']['nb_read_frames']} sha256={value['sha256']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"error: {error}")

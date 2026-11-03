#!/usr/bin/env python3
"""Prepare a local 2x interpolation pilot from the licensed UVG-VCM Karting Time sequence."""

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


DATASET_ID = "uvg-vcm-karting-time-4k60-pilot-v1"
DATASET_PAGE = "https://ultravideo.fi/UVG-VCM/index.html"
RAW_URL = (
    "https://ultravideo.fi/UVG-VCM/KartingTime/"
    "KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv"
)
LICENSE_ID = "CC-BY-4.0"
LICENSE_URL = "https://creativecommons.org/licenses/by/4.0/"
RAW_FILE_NAME = "KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv"
RAW_WIDTH = 3_840
RAW_HEIGHT = 2_160
RAW_FPS = 60
RAW_FRAMES = 70
RAW_BYTES_PER_SAMPLE = 2
RAW_PLANES = 3
RAW_FRAME_BYTES = RAW_WIDTH * RAW_HEIGHT * RAW_PLANES * RAW_BYTES_PER_SAMPLE
RAW_FILE_BYTES = RAW_FRAME_BYTES * RAW_FRAMES
SOURCE_FRAMES = 35
REFERENCE_FRAMES = 69
VALID_MIDPOINT_REFERENCE_INDEXES = list(range(1, 68, 2))
OUTPUT_PROFILES = {"720p": (1_280, 720), "1080p": (1_920, 1_080)}
SOURCE_RATE = "30/1"
REFERENCE_RATE = "60/1"
OUTPUT_PIXEL_FORMAT = "yuv444p10le"
COLOR_TAGS = {
    "primaries": "bt709",
    "transfer": "bt709",
    "matrix": "bt709",
    "range": "tv",
    "pixelFormat": OUTPUT_PIXEL_FORMAT,
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=14_400)
    if result.returncode:
        raise RuntimeError(
            f"command failed with exit code {result.returncode}: {command[0]}\n"
            f"stdout:\n{result.stdout[-2_000:]}\nstderr:\n{result.stderr[-4_000:]}"
        )
    return result


def tool_version(tool: Path) -> str:
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
    streams = document.get("streams", [])
    if len(streams) != 1:
        raise ValueError(f"expected exactly one video stream in {path.name}")
    stream = streams[0]
    stream["duration"] = document.get("format", {}).get("duration")
    return stream


def verify_video(path: Path, stream: dict[str, Any], *, frames: int, rate: str,
                 width: int, height: int) -> None:
    expected = {
        "width": width,
        "height": height,
        "pix_fmt": OUTPUT_PIXEL_FORMAT,
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
    expected_duration = Fraction(frames, int(rate.split("/")[0]))
    actual_duration = Fraction(stream["duration"])
    if abs(actual_duration - expected_duration) > Fraction(1, 1_000):
        raise ValueError(f"{path.name}: duration {actual_duration} differs from expected {expected_duration}")


def ffmpeg_command(
    ffmpeg: Path,
    raw: Path,
    destination: Path,
    *,
    mode: str,
    input_range: str,
    width: int,
    height: int,
    force: bool,
) -> list[str]:
    filters = (
        f"scale={width}:{height}:"
        f"in_color_matrix=bt709:out_color_matrix=bt709:"
        f"in_range={input_range}:out_range=tv:flags=lanczos+accurate_rnd+bitexact,"
        f"format={OUTPUT_PIXEL_FORMAT},"
        "setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709"
    )
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pixel_format", "yuv444p16le",
        "-video_size", f"{RAW_WIDTH}x{RAW_HEIGHT}", "-framerate", str(RAW_FPS),
        "-i", str(raw),
    ]
    if mode == "source":
        command += [
            "-vf", f"select=not(mod(n\\,2)),{filters},settb=expr=1/30,setpts=N",
            "-frames:v", str(SOURCE_FRAMES), "-r", SOURCE_RATE.split("/")[0],
            "-fps_mode", "cfr", "-enc_time_base", "1:30",
        ]
    elif mode == "reference":
        command += [
            "-vf", f"{filters},settb=expr=1/60,setpts=N",
            "-frames:v", str(REFERENCE_FRAMES), "-r", REFERENCE_RATE.split("/")[0],
            "-fps_mode", "cfr", "-enc_time_base", "1:60",
        ]
    else:
        raise ValueError(f"unknown media mode: {mode}")
    command += [
        "-map", "0:v:0", "-an", "-c:v", "ffv1", "-level", "3", "-coder", "1", "-context", "1",
        "-pix_fmt", OUTPUT_PIXEL_FORMAT, "-color_range", "tv", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709", "-f", "matroska",
        "-y" if force else "-n", str(destination),
    ]
    return command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, required=True, help="locally acquired UVG-VCM Karting Time .yuv file")
    parser.add_argument("--output-directory", type=Path, required=True, help="local directory for prepared media")
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--ffprobe", type=Path, required=True)
    parser.add_argument("--output-profile", choices=tuple(OUTPUT_PROFILES), default="1080p")
    parser.add_argument(
        "--source-range", choices=("tv", "pc"), required=True,
        help="explicitly state how the upstream raw YUV code values should be interpreted",
    )
    parser.add_argument("--force", action="store_true", help="replace existing prepared media and report")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    raw = args.raw.resolve(strict=True)
    ffmpeg = args.ffmpeg.resolve(strict=True)
    ffprobe = args.ffprobe.resolve(strict=True)
    output_directory = args.output_directory.resolve()
    output_width, output_height = OUTPUT_PROFILES[args.output_profile]
    if not raw.is_file() or raw.name != RAW_FILE_NAME:
        raise ValueError(f"raw input must be the official {RAW_FILE_NAME} asset")
    if raw.stat().st_size != RAW_FILE_BYTES:
        raise ValueError(f"raw input must contain exactly {RAW_FILE_BYTES} bytes, got {raw.stat().st_size}")
    if not ffmpeg.is_file() or not ffprobe.is_file():
        raise ValueError("FFmpeg and FFprobe must be local executable files")

    output_directory.mkdir(parents=True, exist_ok=True)
    names = {"source": "source-30fps.mkv", "reference": "reference-60fps-valid-window.mkv"}
    final_paths = {key: output_directory / value for key, value in names.items()}
    report_path = output_directory / "prepared-media.json"
    existing = [path for path in [*final_paths.values(), report_path] if path.exists()]
    if existing and not args.force:
        raise FileExistsError(f"prepared output already exists: {existing[0]}; pass --force to replace it")

    temporary_paths = {
        key: output_directory / f".{path.stem}.{os.getpid()}.tmp.mkv"
        for key, path in final_paths.items()
    }
    try:
        for key, mode in (("source", "source"), ("reference", "reference")):
            print(f"Preparing {key}: {temporary_paths[key].name}", flush=True)
            run(ffmpeg_command(ffmpeg, raw, temporary_paths[key], mode=mode,
                               input_range=args.source_range, width=output_width,
                               height=output_height, force=False))

        probed = {key: probe_video(ffprobe, path) for key, path in temporary_paths.items()}
        verify_video(temporary_paths["source"], probed["source"], frames=SOURCE_FRAMES,
                     rate=SOURCE_RATE, width=output_width, height=output_height)
        verify_video(temporary_paths["reference"], probed["reference"],
                     frames=REFERENCE_FRAMES, rate=REFERENCE_RATE,
                     width=output_width, height=output_height)

        ffmpeg_version = tool_version(ffmpeg)
        ffprobe_version = tool_version(ffprobe)
        raw_sha256 = sha256_file(raw)
        for key, temporary_path in temporary_paths.items():
            os.replace(temporary_path, final_paths[key])

        prepared = {
            "schemaVersion": 1,
            "datasetId": DATASET_ID,
            "dataset": "UVG-VCM",
            "sequence": "Karting Time",
            "datasetPage": DATASET_PAGE,
            "rawSource": {
                "url": RAW_URL,
                "file": raw.name,
                "bytes": raw.stat().st_size,
                "sha256": raw_sha256,
                "upstreamChecksumPublished": False,
                "license": LICENSE_ID,
                "licenseUrl": LICENSE_URL,
                "requiredCitation": (
                    "T. Partanen, M. Anttila, R. Kortelahti, G. Gautier, A. Mercat, and J. Vanne, "
                    "\"UVG-VCM: Benchmarking Dataset for Machine-Oriented Visual Data Compression,\" "
                    "Proc. Int. Conf. Qual. Multimedia Exper., Cardiff, United Kingdom, Jun.–Jul. 2026."
                ),
                "upstreamFormat": {
                    "width": RAW_WIDTH,
                    "height": RAW_HEIGHT,
                    "frameRate": REFERENCE_RATE,
                    "frameCount": RAW_FRAMES,
                    "pixelFormat": "yuv444p16le",
                },
            },
            "evaluationTransform": {
                "sourceRangeAssumption": args.source_range,
                "sourceMatrixAssumption": "bt709",
                "sourceTransferAssumption": "bt709",
                "sourcePrimariesAssumption": "bt709",
                "sourceMetadataPublished": False,
                "outputRange": "tv",
                "outputColorTags": COLOR_TAGS,
                "outputProfile": args.output_profile,
                "outputSize": {"width": output_width, "height": output_height},
                "scaler": "FFmpeg scale flags=lanczos+accurate_rnd+bitexact",
                "sourceFrames": "even upstream frame indexes 0..68, retimed to 30/1 fps",
                "referenceFrames": "upstream frame indexes 0..68 at 60/1 fps",
                "validMidpointReferenceIndexes": VALID_MIDPOINT_REFERENCE_INDEXES,
                "validMidpointCount": len(VALID_MIDPOINT_REFERENCE_INDEXES),
                "excludedUpstreamFrameIndexes": [69],
                "exclusionReason": "frame 69 is after the final input frame and is not bracketed for interpolation",
                "precisionNote": "The source page states 16-bit YUV but does not publish color range, matrix, transfer, or primaries; the explicit assumptions above define this SDR benchmark transform, not verified camera metadata.",
            },
            "preparer": {
                "path": str(Path(__file__).resolve()),
                "sha256": sha256_file(Path(__file__).resolve()),
            },
            "ffmpeg": {"path": str(ffmpeg), "version": ffmpeg_version, "sha256": sha256_file(ffmpeg)},
            "ffprobe": {"path": str(ffprobe), "version": ffprobe_version, "sha256": sha256_file(ffprobe)},
            "files": {},
            "limitations": [
                "This is a 70-frame, approximately 1.17-second single-scene pilot, not a representative video ranking.",
                "The fixed SDR BT.709 conversion is an evaluation assumption because the upstream raw file has no published colorimetry metadata.",
                "Only 34 bracketed midpoint samples are valid; the final unbracketed high-rate frame is excluded.",
                "The dataset is machine-vision oriented and does not cover the full consumer-video content mix.",
                "A model comparison still needs blind visual review and additional licensed real footage.",
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
        with tempfile.NamedTemporaryFile("wb", dir=output_directory, prefix=".prepared-media.",
                                         suffix=".tmp", delete=False) as stream:
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
    print(f"raw_sha256={raw_sha256}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, ValueError, RuntimeError, OSError,
            subprocess.TimeoutExpired) as error:
        raise SystemExit(f"error: {error}")

#!/usr/bin/env python3
"""Prepare a local 2x or 4x interpolation pilot from licensed UVG-VCM raw YUV."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from fractions import Fraction
from pathlib import Path
from typing import Any


DATASET_ID_PREFIX = "uvg-vcm"
DATASET_PAGE = "https://ultravideo.fi/UVG-VCM/index.html"
DEFAULT_SEQUENCE = "Karting Time"
DEFAULT_RAW_URL = (
    "https://ultravideo.fi/UVG-VCM/KartingTime/"
    "KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv"
)
LICENSE_ID = "CC-BY-4.0"
LICENSE_URL = "https://creativecommons.org/licenses/by/4.0/"
RAW_WIDTH = 3_840
RAW_HEIGHT = 2_160
RAW_FPS = 60
RAW_BYTES_PER_SAMPLE = 2
RAW_PLANES = 3
RAW_FRAME_BYTES = RAW_WIDTH * RAW_HEIGHT * RAW_PLANES * RAW_BYTES_PER_SAMPLE
OUTPUT_PROFILES = {"720p": (1_280, 720), "1080p": (1_920, 1_080)}
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
    input_rate: int,
    sample_stride: int,
    source_frames: int,
    reference_frames: int,
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
            "-vf", f"select=not(mod(n\\,{sample_stride})),{filters},settb=expr=1/{input_rate},setpts=N",
            "-frames:v", str(source_frames), "-r", str(input_rate),
            "-fps_mode", "cfr", "-enc_time_base", f"1:{input_rate}",
        ]
    elif mode == "reference":
        command += [
            "-vf", f"{filters},settb=expr=1/60,setpts=N",
            "-frames:v", str(reference_frames), "-r", REFERENCE_RATE.split("/")[0],
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
    parser.add_argument("--raw", type=Path, required=True, help="locally acquired UVG-VCM raw YUV segment")
    parser.add_argument("--sequence", default=DEFAULT_SEQUENCE, help="sequence name as listed on the official dataset page")
    parser.add_argument("--raw-url", default=DEFAULT_RAW_URL, help="official UVG-VCM raw sequence URL used to acquire this segment")
    parser.add_argument("--source-start-frame-index", type=int, default=0,
                        help="zero-based first frame index within the full upstream sequence")
    parser.add_argument("--frame-count", type=int, default=70,
                        help="number of consecutive 4K60 frames; must align so the penultimate frame is an input")
    parser.add_argument("--sequence-total-frame-count", type=int,
                        help="total frame count of the full upstream sequence, when published")
    parser.add_argument("--output-directory", type=Path, required=True, help="local directory for prepared media")
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--ffprobe", type=Path, required=True)
    parser.add_argument("--output-profile", choices=tuple(OUTPUT_PROFILES), default="1080p")
    parser.add_argument("--input-rate", type=int, choices=(15, 30), default=30,
                        help="input rate sampled from the 60 fps sequence (30=2x, 15=4x)")
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
    sample_stride = RAW_FPS // args.input_rate
    if not raw.is_file():
        raise ValueError("raw input must be a local regular file")
    if not args.raw_url.startswith("https://ultravideo.fi/UVG-VCM/"):
        raise ValueError("raw URL must point to the official UVG-VCM dataset page")
    if not args.sequence.strip():
        raise ValueError("sequence name must not be empty")
    if args.source_start_frame_index < 0:
        raise ValueError("source start frame index must be non-negative")
    if args.frame_count < sample_stride + 2 or (args.frame_count - 2) % sample_stride:
        raise ValueError(
            f"frame count must be at least {sample_stride + 2} and satisfy "
            f"(frame count - 2) divisible by the {sample_stride}-frame input stride"
        )
    if args.sequence_total_frame_count is not None:
        segment_end = args.source_start_frame_index + args.frame_count
        if args.sequence_total_frame_count < segment_end:
            raise ValueError("the selected raw segment extends past the upstream sequence frame count")
    source_local_indexes = list(range(0, args.frame_count - 1, sample_stride))
    source_frames = len(source_local_indexes)
    reference_frames = args.frame_count - 1
    valid_interpolation_local_indexes = [
        index
        for left_index in range(0, args.frame_count - 2, sample_stride)
        for index in range(left_index + 1, left_index + sample_stride)
    ]
    valid_interpolation_reference_indexes = [
        args.source_start_frame_index + index for index in valid_interpolation_local_indexes
    ]
    valid_midpoint_reference_indexes = [
        args.source_start_frame_index + left_index + sample_stride // 2
        for left_index in range(0, args.frame_count - 2, sample_stride)
    ]
    source_reference_indexes = [args.source_start_frame_index + index for index in source_local_indexes]
    excluded_frame_index = args.source_start_frame_index + args.frame_count - 1
    expected_raw_bytes = RAW_FRAME_BYTES * args.frame_count
    raw_byte_start = RAW_FRAME_BYTES * args.source_start_frame_index
    raw_byte_end = raw_byte_start + expected_raw_bytes - 1
    if raw.stat().st_size != expected_raw_bytes:
        raise ValueError(f"raw input must contain exactly {expected_raw_bytes} bytes for {args.frame_count} frames, got {raw.stat().st_size}")
    if not ffmpeg.is_file() or not ffprobe.is_file():
        raise ValueError("FFmpeg and FFprobe must be local executable files")

    output_directory.mkdir(parents=True, exist_ok=True)
    names = {
        "source": f"source-{args.input_rate}fps.mkv",
        "reference": "reference-60fps-valid-window.mkv",
    }
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
                               input_range=args.source_range, input_rate=args.input_rate,
                               sample_stride=sample_stride, source_frames=source_frames,
                               reference_frames=reference_frames, width=output_width,
                               height=output_height, force=False))

        probed = {key: probe_video(ffprobe, path) for key, path in temporary_paths.items()}
        verify_video(temporary_paths["source"], probed["source"], frames=source_frames,
                     rate=f"{args.input_rate}/1", width=output_width, height=output_height)
        verify_video(temporary_paths["reference"], probed["reference"],
                     frames=reference_frames, rate=REFERENCE_RATE,
                     width=output_width, height=output_height)

        ffmpeg_version = tool_version(ffmpeg)
        ffprobe_version = tool_version(ffprobe)
        raw_sha256 = sha256_file(raw)
        for key, temporary_path in temporary_paths.items():
            os.replace(temporary_path, final_paths[key])

        slug = re.sub(r"[^a-z0-9]+", "-", args.sequence.lower()).strip("-")
        dataset_id = (
            f"{DATASET_ID_PREFIX}-{slug}-frames-{args.source_start_frame_index}-"
            f"{excluded_frame_index}-{args.input_rate}to60fps-v2"
        )
        prepared = {
            "schemaVersion": 2,
            "datasetId": dataset_id,
            "dataset": "UVG-VCM",
            "sequence": args.sequence,
            "datasetPage": DATASET_PAGE,
            "rawSource": {
                "url": args.raw_url,
                "file": raw.name,
                "bytes": raw.stat().st_size,
                "sha256": raw_sha256,
                "upstreamChecksumPublished": False,
                "sourceFrameRangeInclusive": [args.source_start_frame_index, excluded_frame_index],
                "sourceByteRangeInclusive": [raw_byte_start, raw_byte_end],
                "sequenceTotalFrameCount": args.sequence_total_frame_count,
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
                    "segmentFrameCount": args.frame_count,
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
                "inputRate": f"{args.input_rate}/1",
                "referenceRate": REFERENCE_RATE,
                "temporalScale": sample_stride,
                "sourceFrameIndexes": source_reference_indexes,
                "sourceFrames": (
                    f"every {sample_stride} local upstream frame indexes from 0 through {args.frame_count - 2}, "
                    f"representing upstream indexes {source_reference_indexes[0]}..{source_reference_indexes[-1]}, "
                    f"retimed to {args.input_rate}/1 fps"
                ),
                "referenceFrames": (
                    f"local frame indexes 0..{args.frame_count - 2}, representing upstream indexes "
                    f"{args.source_start_frame_index}..{excluded_frame_index - 1} at 60/1 fps"
                ),
                "validInterpolationReferenceIndexes": valid_interpolation_reference_indexes,
                "validInterpolationReferenceLocalIndexes": valid_interpolation_local_indexes,
                "validInterpolationReferenceCount": len(valid_interpolation_reference_indexes),
                "validMidpointReferenceIndexes": valid_midpoint_reference_indexes,
                "validMidpointCount": len(valid_midpoint_reference_indexes),
                "excludedUpstreamFrameIndexes": [excluded_frame_index],
                "exclusionReason": (
                    f"frame {excluded_frame_index} is after the final input frame and is not bracketed for interpolation"
                ),
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
                f"This is a {args.frame_count}-frame segment ({args.frame_count / RAW_FPS:.2f} seconds) from one scene, not a representative video ranking.",
                "The fixed SDR BT.709 conversion is an evaluation assumption because the upstream raw file has no published colorimetry metadata.",
                f"Only {len(valid_interpolation_reference_indexes)} high-rate reference samples are bracketed between input frames; the final unbracketed frame is excluded.",
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

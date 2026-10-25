"""Evaluate a RifeLove result against a local high-frame-rate reference video."""
from __future__ import annotations

import argparse
from bisect import bisect_left
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
from fractions import Fraction


CHUNK_SIZE = 1024 * 1024
FIELDS = ("codec_name", "width", "height", "pix_fmt", "bits_per_raw_sample", "time_base",
          "avg_frame_rate", "color_range", "color_space", "color_transfer", "color_primaries")
ALLOWED_PRIMARIES = {"bt709", "bt470bg", "smpte170m"}
ALLOWED_TRANSFERS = {"bt709", "smpte170m", "iec61966-2-1"}
ALLOWED_MATRICES = {"bt709", "bt470bg", "smpte170m", "gbr"}
ALLOWED_RANGES = {"tv", "mpeg", "pc", "jpeg"}
HDR_TRANSFERS = {"smpte2084", "arib-std-b67"}


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def nearest_distance(sorted_values: list[Fraction], value: Fraction) -> Fraction:
    index = bisect_left(sorted_values, value)
    distances = []
    if index < len(sorted_values):
        distances.append(abs(sorted_values[index] - value))
    if index:
        distances.append(abs(sorted_values[index - 1] - value))
    return min(distances)


def parse_rate(value: str) -> Fraction:
    try:
        rate = Fraction(value)
    except (ValueError, ZeroDivisionError) as error:
        raise argparse.ArgumentTypeError(f"invalid rational target rate: {value}") from error
    if rate <= 0:
        raise argparse.ArgumentTypeError("target rate must be positive")
    return rate


def run(command: list[str], timeout: float, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout, check=False, cwd=cwd)


def probe(ffprobe: Path, path: Path, timeout: float) -> dict:
    result = run([str(ffprobe), "-v", "error", "-select_streams", "v:0", "-show_streams", "-show_frames",
                  "-show_entries", "stream=" + ",".join(FIELDS) + ":frame=best_effort_timestamp",
                  "-of", "json", str(path)], timeout)
    if result.returncode:
        raise RuntimeError(f"ffprobe failed for {path}: {result.stderr[-3000:]}")
    value = json.loads(result.stdout)
    streams = value.get("streams", [])
    if not streams:
        raise ValueError(f"no readable video stream: {path}")
    stream = streams[0]
    try:
        time_base = Fraction(stream["time_base"])
    except (KeyError, ValueError, ZeroDivisionError) as error:
        raise ValueError(f"invalid video time base: {path}") from error
    timestamps = []
    for frame in value.get("frames", []):
        raw = frame.get("best_effort_timestamp")
        if raw is None:
            raise ValueError(f"video frame has no presentation timestamp: {path}")
        try:
            timestamps.append(Fraction(int(raw)) * time_base)
        except (TypeError, ValueError) as error:
            raise ValueError(f"invalid presentation timestamp in {path}") from error
    if not timestamps:
        raise ValueError(f"video has no decodable frames: {path}")
    if any(current <= previous for previous, current in zip(timestamps, timestamps[1:])):
        raise ValueError(f"video presentation timestamps are not strictly increasing: {path}")
    return {"stream": {key: stream.get(key) for key in FIELDS}, "timeBase": str(time_base),
            "timestamps": timestamps}


def validate_profiles(items: list[dict]) -> dict:
    profiles = []
    for item in items:
        stream = item["stream"]
        primaries = stream.get("color_primaries")
        transfer = stream.get("color_transfer")
        matrix = stream.get("color_space")
        color_range = stream.get("color_range")
        if transfer in HDR_TRANSFERS:
            raise ValueError("HDR reference comparison needs a separately validated metric/color pipeline")
        if primaries not in ALLOWED_PRIMARIES or transfer not in ALLOWED_TRANSFERS or matrix not in ALLOWED_MATRICES or color_range not in ALLOWED_RANGES:
            raise ValueError("reference comparison requires explicit supported SDR color tags and range")
        profiles.append({"width": stream.get("width"), "height": stream.get("height"),
                         "pixelFormat": stream.get("pix_fmt"), "colorRange": color_range,
                         "colorMatrix": matrix, "colorTransfer": transfer, "colorPrimaries": primaries})
    if any(profile != profiles[0] for profile in profiles[1:]):
        raise ValueError("source, reference and result must have matching dimensions, pixel format and SDR color tags")
    return profiles[0]


def metric_lines(path: Path, required_key: str) -> list[float]:
    values = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            fields = {}
            for item in line.split():
                if ":" in item:
                    key, value = item.split(":", 1)
                    fields[key] = value
            if "n" not in fields:
                continue
            if required_key not in fields:
                raise RuntimeError(f"FFmpeg metric log is missing {required_key}: {path}")
            try:
                frame_number = int(fields["n"])
                score = float(fields[required_key])
            except ValueError as error:
                raise RuntimeError(f"invalid FFmpeg metric log row: {line.strip()}") from error
            if math.isnan(score) or score == -math.inf:
                raise RuntimeError(f"FFmpeg reported an invalid metric value: {line.strip()}")
            if frame_number != len(values) + 1:
                raise RuntimeError(f"FFmpeg metric log skipped or reordered frames: {path}")
            values.append(score)
    if not values:
        raise RuntimeError(f"FFmpeg produced no per-frame metrics: {path}")
    return values


def summarize(values: list[float]) -> dict:
    finite = [value for value in values if math.isfinite(value)]
    return {"frameCount": len(values), "perfectFrameCount": len(values) - len(finite),
            "meanFrameScore": statistics.fmean(finite) if finite else None,
            "medianFrameScore": statistics.median(finite) if finite else None,
            "minimumFrameScore": min(finite) if finite else None,
            "maximumFrameScore": max(finite) if finite else None}


def calculate_metric(ffmpeg: Path, reference: Path, result: Path, work: Path,
                     name: str, filter_name: str, score_key: str, timeout: float) -> list[float]:
    stats_path = work / f"{name}.log"
    graph = f"[0:v:0][1:v:0]{filter_name}=stats_file={stats_path.name}:shortest=1:repeatlast=0"
    command = [str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-i", str(reference), "-i", str(result),
               "-filter_complex", graph, "-an", "-f", "null", "-"]
    completed = run(command, timeout, cwd=work)
    if completed.returncode:
        raise RuntimeError(f"FFmpeg {name.upper()} comparison failed: {completed.stderr[-3000:]}")
    return metric_lines(stats_path, score_key)


def subgroup(values: list[float], indexes: list[int]) -> list[float]:
    return [values[index] for index in indexes]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="original lower-rate source video")
    parser.add_argument("--reference", required=True, type=Path, help="locally available high-frame-rate ground truth")
    parser.add_argument("--result", required=True, type=Path, help="RifeLove output video")
    parser.add_argument("--fps", required=True, type=parse_rate, help="RifeLove target FPS, as an integer or fraction")
    parser.add_argument("--ffmpeg", required=True, type=Path, help="local ffmpeg.exe")
    parser.add_argument("--ffprobe", required=True, type=Path, help="local ffprobe.exe")
    parser.add_argument("--report", required=True, type=Path, help="new path for the local JSON report")
    parser.add_argument("--timeout", type=float, default=14400, help="maximum seconds per probe/metric pass")
    args = parser.parse_args()

    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    for path in (args.source, args.reference, args.result, args.ffmpeg, args.ffprobe):
        if not path.is_file():
            parser.error(f"required local file not found: {path}")
    report_path = args.report.absolute()
    if report_path.exists():
        parser.error(f"report already exists; choose a new path: {report_path}")
    sources = [args.source.resolve(), args.reference.resolve(), args.result.resolve()]
    if len(set(sources)) != 3:
        parser.error("source, reference and result must be separate files")

    before_hashes = {str(path): digest_file(path) for path in sources}
    timelines = [probe(args.ffprobe, path, args.timeout) for path in sources]
    profile = validate_profiles(timelines)
    source_pts, reference_pts, result_pts = [item["timestamps"] for item in timelines]
    if reference_pts != result_pts:
        raise ValueError("reference and RifeLove result must have identical frame count and presentation timestamps")
    target_pts = [source_pts[0] + Fraction(index, 1) / args.fps for index in range(len(result_pts))]
    time_base = Fraction(timelines[2]["timeBase"])
    timestamp_errors = [abs(actual - expected) for actual, expected in zip(result_pts, target_pts)]
    maximum_timestamp_error = max(timestamp_errors)
    timestamp_tolerance = time_base / 2
    if maximum_timestamp_error > timestamp_tolerance:
        raise ValueError("RifeLove result PTS differs from the requested target-rate grid by more than half an output tick")
    # Container time-base quantization may move a source-aligned output tick
    # away from the source PTS by less than half an output tick. Classify the
    # decoded output sample against its actual PTS, not the ideal rational grid.
    source_indexes = [index for index, pts in enumerate(result_pts)
                      if nearest_distance(source_pts, pts) <= timestamp_tolerance]
    source_index_set = set(source_indexes)
    non_source_indexes = [index for index in range(len(result_pts)) if index not in source_index_set]
    if not non_source_indexes:
        raise ValueError("reference/result contain no frames at times absent from the source")
    represented_source_frames = sum(1 for pts in source_pts
                                    if nearest_distance(result_pts, pts) <= timestamp_tolerance)

    report_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rifelove-quality-") as temporary:
        work = Path(temporary)
        psnr = calculate_metric(args.ffmpeg.resolve(), sources[1], sources[2], work,
                                "psnr", "psnr", "psnr_avg", args.timeout)
        ssim = calculate_metric(args.ffmpeg.resolve(), sources[1], sources[2], work,
                                "ssim", "ssim", "All", args.timeout)
    if len(psnr) != len(result_pts) or len(ssim) != len(result_pts):
        raise RuntimeError("FFmpeg metric frame count differs from the validated video timeline")
    after_hashes = {str(path): digest_file(path) for path in sources}
    if before_hashes != after_hashes:
        raise RuntimeError("one or more source/reference/result videos changed during evaluation")

    report = {
        "schemaVersion": 1,
        "status": "completed",
        "method": {"metrics": ["PSNR", "SSIM"], "comparison": "FFmpeg per-frame metrics on exact reference/result PTS matches",
                   "sourceCoincidentAndNonSourceSamplesReportedSeparately": True,
                   "colorTransformApplied": False, "perceptualBlindReviewPerformed": False},
        "files": {"source": {"path": str(sources[0]), "sha256": before_hashes[str(sources[0])]},
                  "reference": {"path": str(sources[1]), "sha256": before_hashes[str(sources[1])]},
                  "result": {"path": str(sources[2]), "sha256": before_hashes[str(sources[2])]}},
        "profile": profile,
        "timeline": {"sourceFrames": len(source_pts), "referenceFrames": len(reference_pts),
                     "resultFrames": len(result_pts), "targetRate": str(args.fps),
                     "sourceCoincidentFrames": len(source_indexes), "nonSourceSamples": len(non_source_indexes),
                     "sourceFramesRepresentedOnTargetGrid": represented_source_frames,
                     "firstPts": str(result_pts[0]), "lastPts": str(result_pts[-1]),
                     "maximumTargetGridError": str(maximum_timestamp_error),
                     "halfOutputTickTolerance": str(timestamp_tolerance),
                     "referenceAndResultPtsIdentical": True},
        "metrics": {"psnrDb": {"allFrames": summarize(psnr),
                                "sourceCoincidentFrames": summarize(subgroup(psnr, source_indexes)),
                                "nonSourceSamples": summarize(subgroup(psnr, non_source_indexes))},
                    "ssim": {"allFrames": summarize(ssim),
                             "sourceCoincidentFrames": summarize(subgroup(ssim, source_indexes)),
                             "nonSourceSamples": summarize(subgroup(ssim, non_source_indexes))}},
        "inputHashesUnchanged": True,
        "limits": ["PSNR/SSIM depend on the supplied reference and do not replace visual review.",
                   "This tool accepts matching explicitly tagged SDR profiles only; HDR metrics need a validated color-managed path."],
    }
    descriptor, temporary_name = tempfile.mkstemp(prefix=report_path.name + ".", suffix=".tmp", dir=report_path.parent)
    temporary_report = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_report, report_path)
    finally:
        if temporary_report.exists():
            temporary_report.unlink()
    print(json.dumps({"event": "quality_evaluation_completed", "report": str(report_path),
                      "nonSourceSamples": len(non_source_indexes),
                      "nonSourceMeanPsnrDb": report["metrics"]["psnrDb"]["nonSourceSamples"]["meanFrameScore"],
                      "nonSourceMeanSsim": report["metrics"]["ssim"]["nonSourceSamples"]["meanFrameScore"]},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

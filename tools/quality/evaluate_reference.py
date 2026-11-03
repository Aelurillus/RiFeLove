"""Evaluate one or more RifeLove outputs against a local high-frame-rate reference video."""
from __future__ import annotations

import argparse
from array import array
from bisect import bisect_left
import hashlib
import heapq
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


def reported_path_matches(value: object, expected: Path) -> bool | None:
    if not isinstance(value, str) or not value.strip():
        return None
    try:
        reported = Path(value)
        if not reported.is_absolute():
            return None
        return os.path.normcase(str(reported.resolve())) == os.path.normcase(str(expected.resolve()))
    except (OSError, RuntimeError, ValueError):
        return None


def candidate_processing_metadata(output_path: Path, source_path: Path) -> dict:
    sidecar = Path(str(output_path) + ".json")
    if not sidecar.is_file():
        return {"status": "not_found", "sidecarPath": str(sidecar)}
    try:
        with sidecar.open("rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
    except OSError as error:
        return {"status": "invalid", "sidecarPath": str(sidecar), "error": str(error)[:500]}
    if len(raw) > 1024 * 1024:
        return {"status": "too_large", "sidecarPath": str(sidecar), "sizeBytes": len(raw)}
    sidecar_hash = hashlib.sha256(raw).hexdigest()
    try:
        value = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError) as error:
        return {"status": "invalid", "sidecarPath": str(sidecar), "sidecarSha256": sidecar_hash,
                "sidecarSizeBytes": len(raw), "error": str(error)[:500]}
    if not isinstance(value, dict):
        return {"status": "invalid", "sidecarPath": str(sidecar), "sidecarSha256": sidecar_hash,
                "sidecarSizeBytes": len(raw), "error": "report root must be an object"}
    application = value.get("application") if isinstance(value.get("application"), dict) else {}
    if (application.get("name") != "RifeLove" or not isinstance(value.get("model"), dict) or
            not isinstance(value.get("output"), dict)):
        return {"status": "unrecognized", "sidecarPath": str(sidecar), "sidecarSha256": sidecar_hash,
                "sidecarSizeBytes": len(raw)}

    source = value.get("source") if isinstance(value.get("source"), dict) else {}
    output = value.get("output") if isinstance(value.get("output"), dict) else {}
    model = value.get("model") if isinstance(value.get("model"), dict) else {}
    encoding = value.get("encoding") if isinstance(value.get("encoding"), dict) else {}
    gpu = value.get("gpu") if isinstance(value.get("gpu"), dict) else {}
    adapter = model.get("adapter") if isinstance(model.get("adapter"), dict) else None
    source_match = reported_path_matches(source.get("path"), source_path)
    output_match = reported_path_matches(output.get("path"), output_path)
    if source_match is False or output_match is False:
        binding_status = "path_mismatch"
    elif source_match is True and output_match is True:
        binding_status = "paths_match"
    else:
        binding_status = "path_unverified"
    return {
        "status": "parsed", "sidecarPath": str(sidecar), "sidecarSha256": sidecar_hash,
        "sidecarSizeBytes": len(raw), "bindingStatus": binding_status,
        "bindingStrength": "self-reported paths only; the report hash does not authenticate its publisher or model files",
        "sourcePathMatches": source_match, "outputPathMatches": output_match,
        "schemaVersion": value.get("schemaVersion"), "application": value.get("application"),
        "model": {key: model[key] for key in ("id", "version", "sha256", "backend", "fallbackReason") if key in model},
        "adapter": {key: adapter[key] for key in ("id", "version") if key in adapter} if adapter else None,
        "sourcePath": source.get("path"),
        "output": {key: output[key] for key in ("path", "targetFrameRate", "firstTimestamp", "endExclusive",
                                                   "submittedFrames", "width", "height") if key in output},
        "encoding": {key: encoding[key] for key in ("requestedEncoder", "actualEncoder", "pixelFormat", "fallbackReason") if key in encoding},
        "gpu": {key: gpu[key] for key in ("name", "computeCapability", "driverCudaApiVersion") if key in gpu}
    }


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


def parse_candidate(value: str) -> tuple[str, Path]:
    name, separator, raw_path = value.partition("=")
    name = name.strip()
    raw_path = raw_path.strip()
    try:
        name_bytes = len(name.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise argparse.ArgumentTypeError("candidate name must be valid UTF-8") from error
    if not separator or not name or not raw_path or name_bytes > 512 or any(ord(character) < 32 for character in name):
        raise argparse.ArgumentTypeError("candidate must use NAME=PATH with a non-empty single-line name of at most 512 UTF-8 bytes and a path")
    return name, Path(raw_path)


def run(command: list[str], timeout: float, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout, check=False, cwd=cwd)


def executable_version(path: Path, timeout: float) -> str:
    result = run([str(path), "-version"], timeout)
    if result.returncode:
        raise RuntimeError(f"could not read version from {path}: {result.stderr[-1000:]}")
    lines = (result.stdout or result.stderr).splitlines()
    version = next((line.strip() for line in lines if line.strip()), "")
    if not version:
        raise RuntimeError(f"version output is empty for {path}")
    return version[:500]


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
    if not values:
        return {"frameCount": 0, "perfectFrameCount": 0,
                "meanFrameScore": None, "medianFrameScore": None,
                "minimumFrameScore": None, "maximumFrameScore": None}
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


def calculate_temporal_metric(ffmpeg: Path, reference: Path, result: Path, work: Path,
                              name: str, filter_name: str, score_key: str,
                              timeout: float) -> list[float]:
    """Compare the N-1 frame-to-frame change images emitted by tblend."""
    stats_path = work / f"{name}.log"
    graph = ("[0:v:0]tblend=all_mode=grainextract[reference];"
             "[1:v:0]tblend=all_mode=grainextract[result];"
             f"[reference][result]{filter_name}=stats_file={stats_path.name}:shortest=1:repeatlast=0")
    command = [str(ffmpeg), "-hide_banner", "-nostdin", "-v", "error", "-i", str(reference), "-i", str(result),
               "-filter_complex", graph, "-an", "-f", "null", "-"]
    completed = run(command, timeout, cwd=work)
    if completed.returncode:
        raise RuntimeError(f"FFmpeg temporal {name.upper()} comparison failed: {completed.stderr[-3000:]}")
    return metric_lines(stats_path, score_key)


def subgroup(values: list[float], indexes: list[int]) -> list[float]:
    return [values[index] for index in indexes]


def review_shortlist(psnr: list[float], ssim: list[float], temporal_psnr: list[float],
                     temporal_ssim: list[float], result_pts: list[Fraction],
                     source_indexes: list[int], limit: int) -> dict:
    source_index_set = set(source_indexes)
    origin = result_pts[0]
    frame_indexes = sorted((index for index, score in enumerate(psnr) if math.isfinite(score)),
                           key=lambda index: (psnr[index], index))[:limit]
    transition_indexes = sorted((index for index, score in enumerate(temporal_psnr) if math.isfinite(score)),
                                key=lambda index: (temporal_psnr[index], index))[:limit]
    return {
        "frameScoreRankedByLowestPsnrDb": [
            {"frameIndex": index, "pts": str(result_pts[index]),
             "relativeTime": str(result_pts[index] - origin),
             "sampleType": "sourceCoincident" if index in source_index_set else "nonSource",
             "psnrDb": psnr[index], "ssim": ssim[index]}
            for index in frame_indexes],
        "transitionScoreRankedByLowestPsnrDb": [
            {"transitionIndex": index,
             "startPts": str(result_pts[index]), "endPts": str(result_pts[index + 1]),
             "startRelativeTime": str(result_pts[index] - origin),
             "endRelativeTime": str(result_pts[index + 1] - origin),
             "touchesNonSourceSample": index not in source_index_set or index + 1 not in source_index_set,
             "temporalDifferencePsnrDb": temporal_psnr[index],
             "temporalDifferenceSsim": temporal_ssim[index]}
            for index in transition_indexes],
        "frameIndexBase": 0,
        "transitionIndexBase": 0,
        "limitPerList": limit,
        "interpretation": "Lowest PSNR samples are review cues, not an automatic artifact or flicker detector."
    }


def candidate_timeline(source_pts: list[Fraction], result_pts: list[Fraction], time_base: Fraction,
                       target_rate: Fraction) -> tuple[dict, list[int], list[int]]:
    target_pts = [source_pts[0] + Fraction(index, 1) / target_rate for index in range(len(result_pts))]
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
    generated_transition_indexes = [index - 1 for index in range(1, len(result_pts))
                                    if index not in source_index_set or index - 1 not in source_index_set]
    source_only_transition_indexes = [index - 1 for index in range(1, len(result_pts))
                                      if index in source_index_set and index - 1 in source_index_set]
    timeline = {"sourceFrames": len(source_pts), "referenceFrames": len(result_pts),
                "resultFrames": len(result_pts), "targetRate": str(target_rate),
                "sourceCoincidentFrames": len(source_indexes), "nonSourceSamples": len(non_source_indexes),
                "sourceFramesRepresentedOnTargetGrid": represented_source_frames,
                "interFrameTransitions": len(result_pts) - 1,
                "transitionsTouchingNonSourceSamples": len(generated_transition_indexes),
                "sourceToSourceTransitions": len(source_only_transition_indexes),
                "firstPts": str(result_pts[0]), "lastPts": str(result_pts[-1]),
                "maximumTargetGridError": str(maximum_timestamp_error),
                "halfOutputTickTolerance": str(timestamp_tolerance),
                "referenceAndResultPtsIdentical": True}
    return timeline, source_indexes, non_source_indexes


def candidate_metrics(ffmpeg: Path, reference: Path, result: Path, work: Path, index: int,
                      result_pts: list[Fraction], source_indexes: list[int], non_source_indexes: list[int],
                      timeout: float, review_sample_count: int) -> tuple[dict, dict, dict]:
    prefix = f"candidate-{index}"
    psnr = calculate_metric(ffmpeg, reference, result, work, f"{prefix}-psnr", "psnr", "psnr_avg", timeout)
    ssim = calculate_metric(ffmpeg, reference, result, work, f"{prefix}-ssim", "ssim", "All", timeout)
    temporal_psnr = calculate_temporal_metric(ffmpeg, reference, result, work,
                                              f"{prefix}-temporal-psnr", "psnr", "psnr_avg", timeout)
    temporal_ssim = calculate_temporal_metric(ffmpeg, reference, result, work,
                                              f"{prefix}-temporal-ssim", "ssim", "All", timeout)
    if len(psnr) != len(result_pts) or len(ssim) != len(result_pts):
        raise RuntimeError("FFmpeg metric frame count differs from the validated video timeline")
    expected_transition_count = len(result_pts) - 1
    if len(temporal_psnr) != expected_transition_count or len(temporal_ssim) != expected_transition_count:
        raise RuntimeError("FFmpeg temporal metric frame count differs from the validated inter-frame timeline")
    source_index_set = set(source_indexes)
    generated_transition_indexes = [index - 1 for index in range(1, len(result_pts))
                                    if index not in source_index_set or index - 1 not in source_index_set]
    source_only_transition_indexes = [index - 1 for index in range(1, len(result_pts))
                                      if index in source_index_set and index - 1 in source_index_set]
    metrics = {"psnrDb": {"allFrames": summarize(psnr),
                           "sourceCoincidentFrames": summarize(subgroup(psnr, source_indexes)),
                           "nonSourceSamples": summarize(subgroup(psnr, non_source_indexes))},
               "ssim": {"allFrames": summarize(ssim),
                        "sourceCoincidentFrames": summarize(subgroup(ssim, source_indexes)),
                        "nonSourceSamples": summarize(subgroup(ssim, non_source_indexes))},
               "temporalDifferencePsnrDb": {
                   "allTransitions": summarize(temporal_psnr),
                   "transitionsTouchingNonSourceSamples": summarize(subgroup(temporal_psnr, generated_transition_indexes)),
                   "sourceToSourceTransitions": summarize(subgroup(temporal_psnr, source_only_transition_indexes))},
               "temporalDifferenceSsim": {
                   "allTransitions": summarize(temporal_ssim),
                   "transitionsTouchingNonSourceSamples": summarize(subgroup(temporal_ssim, generated_transition_indexes)),
                   "sourceToSourceTransitions": summarize(subgroup(temporal_ssim, source_only_transition_indexes))}}
    review = review_shortlist(psnr, ssim, temporal_psnr, temporal_ssim,
                              result_pts, source_indexes, review_sample_count)
    score_vectors = {"psnr": psnr, "ssim": ssim,
                     "temporalPsnr": temporal_psnr, "temporalSsim": temporal_ssim}
    return metrics, review, score_vectors


def load_shot_manifest(path: Path, source_hash: str, reference_hash: str,
                       source_pts: list[Fraction], reference_pts: list[Fraction],
                       source_time_base: Fraction, reference_time_base: Fraction,
                       target_rate: Fraction) -> tuple[dict, list[int], list[int]]:
    try:
        with path.open("rb") as stream:
            raw = stream.read(1024 * 1024 + 1)
    except OSError as error:
        raise ValueError(f"could not read shot manifest: {error}") from error
    if len(raw) > 1024 * 1024:
        raise ValueError("shot manifest exceeds the 1 MiB limit")
    try:
        manifest = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"shot manifest is not valid UTF-8 JSON: {error}") from error
    if (not isinstance(manifest, dict) or type(manifest.get("schemaVersion")) is not int or
            manifest.get("schemaVersion") != 1):
        raise ValueError("shot manifest must be an object with schemaVersion 1")
    dataset_id = manifest.get("datasetId")
    try:
        dataset_id_size = len(dataset_id.encode("utf-8")) if isinstance(dataset_id, str) else 0
    except UnicodeEncodeError as error:
        raise ValueError("shot manifest datasetId must be valid UTF-8") from error
    if not isinstance(dataset_id, str) or not dataset_id.strip() or dataset_id_size > 256:
        raise ValueError("shot manifest datasetId must be a non-empty string of at most 256 UTF-8 bytes")
    if type(manifest.get("frameIndexBase")) is not int or manifest.get("frameIndexBase") != 0:
        raise ValueError("shot manifest frameIndexBase must be 0")
    if manifest.get("sourceSha256") != source_hash or manifest.get("referenceSha256") != reference_hash:
        raise ValueError("shot manifest source/reference SHA-256 does not match the evaluated files")
    if (type(manifest.get("frameCount")) is not int or
            manifest.get("frameCount") != len(reference_pts)):
        raise ValueError("shot manifest frameCount does not match the reference timeline")
    rate_value = manifest.get("frameRate")
    if not isinstance(rate_value, str) or not rate_value or len(rate_value) > 64:
        raise ValueError("shot manifest frameRate must be a positive rational string")
    try:
        manifest_rate = Fraction(rate_value)
    except (TypeError, ValueError, ZeroDivisionError) as error:
        raise ValueError("shot manifest frameRate must be a positive rational") from error
    if manifest_rate <= 0 or manifest_rate != target_rate:
        raise ValueError("shot manifest frameRate must match the requested target rate")
    expected_step = 1 / manifest_rate
    reference_origin = reference_pts[0]
    if any(abs(timestamp - (reference_origin + index * expected_step)) > reference_time_base / 2
           for index, timestamp in enumerate(reference_pts)):
        raise ValueError("shot manifest requires a CFR reference matching its declared frameRate")

    starts = manifest.get("shotStarts")
    if (not isinstance(starts, list) or not starts or starts[0] != 0 or
            len(starts) > 10000 or
            any(type(index) is not int for index in starts) or
            any(index < 0 or index >= len(reference_pts) for index in starts) or
            any(current <= previous for previous, current in zip(starts, starts[1:]))):
        raise ValueError("shotStarts must be increasing zero-based reference indexes beginning at 0")

    # Every source timestamp must land on a reference frame so the manifest can
    # classify the actual endpoint frames used to generate each non-source sample.
    source_reference_indexes = []
    source_alignment_tolerance = (source_time_base + reference_time_base) / 2
    for timestamp in source_pts:
        insertion = bisect_left(reference_pts, timestamp)
        nearby = [index for index in (insertion - 1, insertion) if 0 <= index < len(reference_pts)]
        nearest = min(nearby, key=lambda index: abs(reference_pts[index] - timestamp))
        if abs(reference_pts[nearest] - timestamp) > source_alignment_tolerance:
            raise ValueError("shot manifest requires every source PTS to align with a reference frame")
        source_reference_indexes.append(nearest)
    if any(current <= previous for previous, current in zip(source_reference_indexes, source_reference_indexes[1:])):
        raise ValueError("source PTS do not map to distinct increasing reference frames")

    frame_shots = [0] * len(reference_pts)
    for shot_index, start in enumerate(starts):
        end = starts[shot_index + 1] if shot_index + 1 < len(starts) else len(reference_pts)
        frame_shots[start:end] = [shot_index] * (end - start)
    return ({"schemaVersion": 1, "datasetId": dataset_id.strip(),
             "frameRate": str(manifest_rate), "frameCount": len(reference_pts),
             "frameIndexBase": 0, "shotStarts": starts,
             "shotCount": len(starts)}, frame_shots, source_reference_indexes)


def shot_analysis(shot_starts: list[int], frame_shots: list[int],
                  source_pts: list[Fraction], result_pts: list[Fraction],
                  source_reference_indexes: list[int], source_indexes: list[int],
                  vectors: dict) -> dict:
    def encode_metric(value: float) -> float | str:
        if math.isfinite(value):
            return value
        if value == math.inf:
            return "perfect"
        raise ValueError("shot analysis contains an invalid metric value")

    shot_count = len(shot_starts)
    source_index_set = set(source_indexes)
    eligible_frames: list[list[int]] = [[] for _ in range(shot_count)]
    eligible_non_source: list[list[int]] = [[] for _ in range(shot_count)]
    crossed_samples = []
    unbracketed_samples = []

    for frame_index, timestamp in enumerate(result_pts):
        shot_index = frame_shots[frame_index]
        if frame_index in source_index_set:
            eligible_frames[shot_index].append(frame_index)
            continue
        left_source = bisect_left(source_pts, timestamp) - 1
        right_source = left_source + 1
        if left_source < 0 or right_source >= len(source_pts):
            unbracketed_samples.append(frame_index)
            continue
        left_reference = source_reference_indexes[left_source]
        right_reference = source_reference_indexes[right_source]
        left_shot = frame_shots[left_reference]
        right_shot = frame_shots[right_reference]
        if left_shot != right_shot or shot_index != left_shot:
            crossed_samples.append({"frameIndex": frame_index, "pts": str(timestamp),
                                    "relativeTime": str(timestamp - result_pts[0]),
                                    "referenceShotIndex": shot_index,
                                    "leftSourceFrameIndex": left_reference,
                                    "leftSourceShotIndex": left_shot,
                                    "rightSourceFrameIndex": right_reference,
                                    "rightSourceShotIndex": right_shot,
                                    "psnrDb": encode_metric(vectors["psnr"][frame_index]),
                                    "ssim": encode_metric(vectors["ssim"][frame_index])})
            continue
        eligible_frames[shot_index].append(frame_index)
        eligible_non_source[shot_index].append(frame_index)

    shots = []
    for shot_index, start in enumerate(shot_starts):
        end = shot_starts[shot_index + 1] if shot_index + 1 < shot_count else len(frame_shots)
        frame_indexes = eligible_frames[shot_index]
        frame_index_set = set(frame_indexes)
        non_source_indexes = eligible_non_source[shot_index]
        transitions = [index for index in range(start, end - 1)
                       if index in frame_index_set and index + 1 in frame_index_set]
        shots.append({"shotIndex": shot_index, "startReferenceFrameIndex": start,
                      "endReferenceFrameIndexExclusive": end,
                      "referenceFrameCount": end - start,
                      "eligibleFrameSamples": len(frame_indexes),
                      "sourceCoincidentFrames": len(frame_indexes) - len(non_source_indexes),
                      "intraShotNonSourceSamples": len(non_source_indexes),
                      "intraShotMetrics": {
                          "psnrDb": {"allEligibleFrames": summarize(subgroup(vectors["psnr"], frame_indexes)),
                                     "nonSourceSamples": summarize(subgroup(vectors["psnr"], non_source_indexes))},
                          "ssim": {"allEligibleFrames": summarize(subgroup(vectors["ssim"], frame_indexes)),
                                   "nonSourceSamples": summarize(subgroup(vectors["ssim"], non_source_indexes))},
                          "temporalDifferencePsnrDb": summarize(subgroup(vectors["temporalPsnr"], transitions)),
                          "temporalDifferenceSsim": summarize(subgroup(vectors["temporalSsim"], transitions))},
                      "intraShotTransitionCount": len(transitions)})

    boundary_transitions = [{"transitionIndex": start - 1,
                             "fromShotIndex": frame_shots[start - 1],
                             "toShotIndex": frame_shots[start],
                             "temporalDifferencePsnrDb": encode_metric(vectors["temporalPsnr"][start - 1]),
                             "temporalDifferenceSsim": encode_metric(vectors["temporalSsim"][start - 1])}
                            for start in shot_starts[1:]]
    return {"frameIndexBase": 0,
            "frameIndexDomain": "reference and result timeline",
            "shots": shots,
            "crossShotInterpolationSamples": crossed_samples,
            "unbracketedNonSourceFrameIndexes": unbracketed_samples,
            "transitionsAcrossShotBoundaries": boundary_transitions,
            "interpretation": "Per-shot frame metrics include source-coincident frames and only non-source samples whose bracketing source frames belong to that same shot. Cross-shot interpolation samples and transitions are reported separately; temporal-difference metrics are not motion-compensated."}


def write_score_vectors(path: Path, vectors: dict) -> None:
    with path.open("wb") as stream:
        for name in ("psnr", "ssim", "temporalPsnr", "temporalSsim"):
            array("d", vectors[name]).tofile(stream)


def shared_review_shortlist(candidates: list[dict], result_pts: list[Fraction],
                            worst_frame_psnr: array, worst_transition_psnr: array,
                            limit: int) -> dict:
    if not candidates:
        raise ValueError("shared review needs at least one candidate")
    origin = result_pts[0]

    def encode_score(value: float) -> float | str:
        if math.isfinite(value):
            return value
        if value == math.inf:
            return "perfect"
        raise ValueError("shared review contains a non-finite metric")

    def encode_finite_score(value: float) -> float:
        if not math.isfinite(value):
            raise ValueError("shared review contains a non-finite SSIM metric")
        return value

    frame_indexes = heapq.nsmallest(limit,
        (index for index, value in enumerate(worst_frame_psnr) if math.isfinite(value)),
        key=lambda index: (worst_frame_psnr[index], index))
    transition_indexes = heapq.nsmallest(limit,
        (index for index, value in enumerate(worst_transition_psnr) if math.isfinite(value)),
        key=lambda index: (worst_transition_psnr[index], index))

    frames_by_index = {}
    transitions_by_index = {}
    frame_count, transition_count = len(result_pts), len(result_pts) - 1
    element_size = array("d").itemsize
    offsets = {"psnr": 0, "ssim": frame_count, "temporalPsnr": frame_count * 2,
               "temporalSsim": frame_count * 2 + transition_count}
    for candidate in candidates:
        with Path(candidate["scorePath"]).open("rb") as stream:
            def score(name: str, index: int) -> float:
                stream.seek((offsets[name] + index) * element_size)
                value = array("d")
                value.fromfile(stream, 1)
                return value[0]

            for index in frame_indexes:
                point = frames_by_index.setdefault(index, {
                    "frameIndex": index, "pts": str(result_pts[index]),
                    "relativeTime": str(result_pts[index] - origin), "candidateScores": {}})
                point["candidateScores"][candidate["name"]] = {
                    "psnrDb": encode_score(score("psnr", index)),
                    "ssim": encode_finite_score(score("ssim", index))}
            for index in transition_indexes:
                point = transitions_by_index.setdefault(index, {
                    "transitionIndex": index,
                    "startPts": str(result_pts[index]), "endPts": str(result_pts[index + 1]),
                    "startRelativeTime": str(result_pts[index] - origin),
                    "endRelativeTime": str(result_pts[index + 1] - origin), "candidateScores": {}})
                point["candidateScores"][candidate["name"]] = {
                    "temporalDifferencePsnrDb": encode_score(score("temporalPsnr", index)),
                    "temporalDifferenceSsim": encode_finite_score(score("temporalSsim", index))}
    frames = [frames_by_index[index] for index in frame_indexes]
    transitions = [transitions_by_index[index] for index in transition_indexes]
    return {"frameScoreRankedByLowestPsnrDb": frames,
            "transitionScoreRankedByLowestPsnrDb": transitions,
            "frameIndexBase": 0, "transitionIndexBase": 0, "limitPerList": limit,
            "ranking": "lowest finite candidate PSNR at each shared timestamp",
            "interpretation": "Every candidate uses the same review timestamps; scores are hidden during blind visual review and remain available for later attribution."}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="original lower-rate source video")
    parser.add_argument("--reference", required=True, type=Path, help="locally available high-frame-rate ground truth")
    result_group = parser.add_mutually_exclusive_group(required=True)
    result_group.add_argument("--result", type=Path, help="one RifeLove output video (single-result report)")
    result_group.add_argument("--candidate", action="append", type=parse_candidate, metavar="NAME=PATH",
                              help="candidate output video; repeat to compare multiple models against one reference")
    parser.add_argument("--fps", required=True, type=parse_rate, help="RifeLove target FPS, as an integer or fraction")
    parser.add_argument("--shot-manifest", type=Path,
                        help="optional hash-bound CFR shot manifest for per-shot metrics and cross-cut exclusions")
    parser.add_argument("--ffmpeg", required=True, type=Path, help="local ffmpeg.exe")
    parser.add_argument("--ffprobe", required=True, type=Path, help="local ffprobe.exe")
    parser.add_argument("--report", required=True, type=Path, help="new path for the local JSON report")
    parser.add_argument("--timeout", type=float, default=14400, help="maximum seconds per probe/metric pass")
    parser.add_argument("--review-samples", type=int, default=12,
                        help="include up to this many lowest-PSNR frames/transitions per candidate, plus shared worst-case timestamps in multi-candidate reports (0-1000)")
    args = parser.parse_args()

    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.review_samples < 0 or args.review_samples > 1000:
        parser.error("--review-samples must be between 0 and 1000")
    if args.candidate is not None and len(args.candidate) < 2:
        parser.error("--candidate mode needs at least two named outputs; use --result for one output")
    if args.candidate is not None and len(args.candidate) > 64:
        parser.error("--candidate mode supports at most 64 outputs")
    review_sample_limit = min(args.review_samples, 5000 // len(args.candidate)) if args.candidate else args.review_samples
    candidate_specs = [("result", args.result)] if args.result is not None else args.candidate
    names = [name.casefold() for name, _ in candidate_specs]
    if len(set(names)) != len(names):
        parser.error("candidate names must be unique, ignoring case")
    required_paths = [args.source, args.reference, *(path for _, path in candidate_specs), args.ffmpeg, args.ffprobe]
    if args.shot_manifest is not None:
        required_paths.append(args.shot_manifest)
    for path in required_paths:
        if not path.is_file():
            parser.error(f"required local file not found: {path}")
    report_path = args.report.absolute()
    if report_path.exists():
        parser.error(f"report already exists; choose a new path: {report_path}")
    paths = [args.source.resolve(), args.reference.resolve(), *(path.resolve() for _, path in candidate_specs)]
    if len(set(paths)) != len(paths):
        parser.error("source, reference and every candidate output must be separate files")

    tool_paths = [args.ffmpeg.resolve(), args.ffprobe.resolve()]
    tracked_paths = list(dict.fromkeys([*paths, *tool_paths]))
    shot_manifest_path = args.shot_manifest.resolve() if args.shot_manifest is not None else None
    if shot_manifest_path is not None:
        tracked_paths.append(shot_manifest_path)
    before_hashes = {str(path): digest_file(path) for path in tracked_paths}
    tool_identities = {name: {"path": str(path), "sha256": before_hashes[str(path)],
                              "version": executable_version(path, min(args.timeout, 30))}
                       for name, path in (("ffmpeg", tool_paths[0]), ("ffprobe", tool_paths[1]))}
    processing_metadata = [candidate_processing_metadata(path, paths[0]) for path in paths[2:]]
    for metadata in processing_metadata:
        if "sidecarSha256" in metadata:
            sidecar_path = Path(metadata["sidecarPath"])
            tracked_paths.append(sidecar_path)
            before_hashes[str(sidecar_path)] = metadata["sidecarSha256"]
    timelines = [probe(args.ffprobe, path, args.timeout) for path in paths]
    profile = validate_profiles(timelines)
    source_pts, reference_pts = timelines[0]["timestamps"], timelines[1]["timestamps"]
    shot_manifest = None
    frame_shots = None
    source_reference_indexes = None
    if shot_manifest_path is not None:
        shot_manifest, frame_shots, source_reference_indexes = load_shot_manifest(
            shot_manifest_path, before_hashes[str(paths[0])], before_hashes[str(paths[1])],
            source_pts, reference_pts, Fraction(timelines[0]["timeBase"]),
            Fraction(timelines[1]["timeBase"]), args.fps)
    for (name, _), timeline in zip(candidate_specs, timelines[2:]):
        if reference_pts != timeline["timestamps"]:
            raise ValueError(f"candidate {name!r} and reference must have identical frame count and presentation timestamps")

    report_path.parent.mkdir(parents=True, exist_ok=True)
    candidate_reports = []
    shared_review = None
    with tempfile.TemporaryDirectory(prefix="rifelove-quality-") as temporary:
        work = Path(temporary)
        candidate_score_spools = []
        worst_frame_psnr = None
        worst_transition_psnr = None
        for index, ((name, _), result_path, result_timeline, metadata) in enumerate(
                zip(candidate_specs, paths[2:], timelines[2:], processing_metadata), start=1):
            result_pts = result_timeline["timestamps"]
            timeline_report, source_indexes, non_source_indexes = candidate_timeline(
                source_pts, result_pts, Fraction(result_timeline["timeBase"]), args.fps)
            metrics, review, score_vectors = candidate_metrics(args.ffmpeg.resolve(), paths[1], result_path, work, index,
                                                                result_pts, source_indexes, non_source_indexes,
                                                                args.timeout, review_sample_limit)
            candidate_report = {"name": name,
                                "output": {"path": str(result_path),
                                           "sha256": before_hashes[str(result_path)]},
                                "timeline": timeline_report, "metrics": metrics,
                                "reviewSamples": review, "processingMetadata": metadata}
            if frame_shots is not None:
                candidate_report["shotAnalysis"] = shot_analysis(
                    shot_manifest["shotStarts"], frame_shots, source_pts, result_pts, source_reference_indexes,
                    source_indexes, score_vectors)
            candidate_reports.append(candidate_report)
            if args.candidate is not None:
                if worst_frame_psnr is None:
                    worst_frame_psnr = array("d", [math.inf]) * len(result_pts)
                    worst_transition_psnr = array("d", [math.inf]) * (len(result_pts) - 1)
                for score_index, value in enumerate(score_vectors["psnr"]):
                    if math.isfinite(value) and value < worst_frame_psnr[score_index]:
                        worst_frame_psnr[score_index] = value
                for score_index, value in enumerate(score_vectors["temporalPsnr"]):
                    if math.isfinite(value) and value < worst_transition_psnr[score_index]:
                        worst_transition_psnr[score_index] = value
                score_path = work / f"candidate-{index}-scores.bin"
                write_score_vectors(score_path, score_vectors)
                candidate_score_spools.append({"name": name, "scorePath": str(score_path)})
                del score_vectors
        if args.candidate is not None:
            shared_review = shared_review_shortlist(candidate_score_spools, reference_pts,
                                                    worst_frame_psnr, worst_transition_psnr,
                                                    review_sample_limit)
    after_hashes = {str(path): digest_file(path) for path in tracked_paths}
    if before_hashes != after_hashes:
        raise RuntimeError("one or more videos, processing reports or evaluation executables changed during evaluation")

    method = {"metrics": ["PSNR", "SSIM", "temporalDifferencePsnrDb", "temporalDifferenceSsim"],
              "comparison": "FFmpeg per-frame metrics on exact reference/output PTS matches",
              "sourceCoincidentAndNonSourceSamplesReportedSeparately": True,
              "temporalDifference": "FFmpeg tblend grainextract compares each consecutive-frame change; N input frames produce N-1 transitions",
              "temporalTransitionsTouchingNonSourceSamplesReportedSeparately": True,
              "reviewShortlist": "Per-candidate lowest finite PSNR frames and transitions plus shared worst-case timestamps for multi-candidate review; not artifact detection",
              "processingMetadata": "Adjacent <video>.json reports are self-reported and path-checked; hashes do not authenticate publisher or model files",
              "shotSegmentation": "When a hash-bound --shot-manifest is supplied, report per-shot metrics and separate non-source samples whose bracketing source frames cross a declared cut",
              "colorTransformApplied": False, "perceptualBlindReviewPerformed": False}
    common_files = {"source": {"path": str(paths[0]), "sha256": before_hashes[str(paths[0])]},
                    "reference": {"path": str(paths[1]), "sha256": before_hashes[str(paths[1])]}}
    if shot_manifest_path is not None:
        common_files["shotManifest"] = {"path": str(shot_manifest_path),
                                        "sha256": before_hashes[str(shot_manifest_path)],
                                        "datasetId": shot_manifest["datasetId"],
                                        "shotCount": shot_manifest["shotCount"]}
    limits = ["PSNR/SSIM depend on the supplied reference and do not replace visual review.",
              "Temporal-difference scores are not motion-compensated and do not by themselves identify perceptual flicker; camera motion, cuts and reference alignment affect them.",
              "This tool accepts matching explicitly tagged SDR profiles only; HDR metrics need a validated color-managed path.",
              "Adjacent RifeLove processing reports are self-reported metadata; path matches and sidecar hashes do not authenticate the producer or model files."]
    if args.result is not None:
        candidate = candidate_reports[0]
        report = {"schemaVersion": 1, "status": "completed", "method": method,
                  "files": {**common_files, "result": candidate["output"]}, "profile": profile,
                  "timeline": candidate["timeline"], "metrics": candidate["metrics"],
                  "reviewSamples": candidate["reviewSamples"],
                  "processingMetadata": candidate["processingMetadata"],
                  "tools": tool_identities,
                  "inputHashesUnchanged": True, "limits": limits}
    else:
        report = {"schemaVersion": 2, "status": "completed", "method": method,
                  "files": common_files, "profile": profile,
                  "tools": tool_identities,
                  "comparison": {"candidateCount": len(candidate_reports),
                                 "allCandidatePtsMatchReference": True,
                                 "targetRate": str(args.fps),
                                 "sharedReviewSamples": shared_review,
                                 "interpretation": "Review each metric and the corresponding frames; no weighted overall score is assigned."},
                  "candidates": candidate_reports, "inputHashesUnchanged": True, "limits": limits}
    descriptor, temporary_name = tempfile.mkstemp(prefix=report_path.name + ".", suffix=".tmp", dir=report_path.parent)
    temporary_report = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        if temporary_report.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("quality report exceeds the desktop import limit of 16 MiB")
        os.replace(temporary_report, report_path)
    finally:
        if temporary_report.exists():
            temporary_report.unlink()
    summaries = [{"name": candidate["name"],
                  "nonSourceSamples": candidate["timeline"]["nonSourceSamples"],
                  "nonSourceMeanPsnrDb": candidate["metrics"]["psnrDb"]["nonSourceSamples"]["meanFrameScore"],
                  "nonSourceMeanSsim": candidate["metrics"]["ssim"]["nonSourceSamples"]["meanFrameScore"],
                  "transitionsTouchingNonSourceSamples": candidate["timeline"]["transitionsTouchingNonSourceSamples"],
                  "meanTemporalDifferencePsnrDb": candidate["metrics"]["temporalDifferencePsnrDb"]
                      ["transitionsTouchingNonSourceSamples"]["meanFrameScore"],
                  "meanTemporalDifferenceSsim": candidate["metrics"]["temporalDifferenceSsim"]
                      ["transitionsTouchingNonSourceSamples"]["meanFrameScore"]}
                 for candidate in candidate_reports]
    summary = {"event": "quality_evaluation_completed", "report": str(report_path)}
    if args.result is not None:
        summary.update({key: value for key, value in summaries[0].items() if key != "name"})
    else:
        summary["candidates"] = summaries
    print(json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

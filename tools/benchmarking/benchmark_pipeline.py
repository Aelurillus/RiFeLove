"""Run repeatable, local RifeLove end-to-end performance measurements.

This tool records throughput and validation evidence. It does not score visual
quality; a reference video and a separate quality-evaluation workflow are
required for perceptual comparisons.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time
from fractions import Fraction


SCHEMA_VERSION = 1
CHUNK_SIZE = 1024 * 1024


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def run_text(command: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=timeout, check=False)


def atomic_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def parse_fraction(value: str) -> Fraction:
    try:
        rate = Fraction(value)
    except (ValueError, ZeroDivisionError) as error:
        raise argparse.ArgumentTypeError(f"invalid rational frame rate: {value}") from error
    if rate <= 0 or rate.numerator > 1_000_000 or rate.denominator > 1_000_000:
        raise argparse.ArgumentTypeError("frame rate must be positive and reasonably bounded")
    return rate


def probe(ffprobe: Path, source: Path, timeout: float = 60) -> dict:
    result = run_text([str(ffprobe), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(source)], timeout)
    if result.returncode:
        raise RuntimeError(f"ffprobe failed for {source}: {result.stderr[-3000:]}")
    return json.loads(result.stdout)


def video_stream(media: dict) -> dict:
    return next((stream for stream in media.get("streams", []) if stream.get("codec_type") == "video"), {})


def source_metadata(ffprobe: Path, source: Path) -> dict:
    media = probe(ffprobe, source)
    stream = video_stream(media)
    if not stream:
        raise ValueError(f"source has no video stream: {source}")
    return {
        "path": str(source),
        "sha256": digest_file(source),
        "sizeBytes": source.stat().st_size,
        "durationSeconds": media.get("format", {}).get("duration"),
        "video": {key: stream.get(key) for key in (
            "index", "codec_name", "width", "height", "pix_fmt", "bits_per_raw_sample",
            "avg_frame_rate", "r_frame_rate", "time_base", "color_range", "color_space",
            "color_transfer", "color_primaries", "field_order")},
        "audioStreams": sum(1 for item in media.get("streams", []) if item.get("codec_type") == "audio"),
        "subtitleStreams": sum(1 for item in media.get("streams", []) if item.get("codec_type") == "subtitle"),
    }


def output_metadata(ffprobe: Path, output: Path, expected_frames: int) -> dict:
    result = run_text([str(ffprobe), "-v", "error", "-count_frames", "-select_streams", "v:0",
                       "-show_entries", "stream=codec_name,width,height,pix_fmt,avg_frame_rate,nb_read_frames",
                       "-of", "json", str(output)], 120)
    if result.returncode:
        raise RuntimeError(f"output ffprobe failed: {result.stderr[-3000:]}")
    streams = json.loads(result.stdout).get("streams", [])
    if not streams:
        raise RuntimeError("output has no decodable video stream")
    stream = streams[0]
    try:
        decoded_frames = int(stream["nb_read_frames"])
    except (KeyError, TypeError, ValueError) as error:
        raise RuntimeError("ffprobe did not report a decoded output frame count") from error
    if decoded_frames != expected_frames:
        raise RuntimeError(f"CLI reported {expected_frames} frames but ffprobe decoded {decoded_frames}")
    return {"decodedFrames": decoded_frames, **{key: stream.get(key) for key in (
        "codec_name", "width", "height", "pix_fmt", "avg_frame_rate")}}


def safe_stem(source: Path) -> str:
    value = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", source.stem).strip(" .")
    return (value or "video")[:48]


def summarize(samples: list[float]) -> dict | None:
    if not samples:
        return None
    return {"count": len(samples), "median": statistics.median(samples),
            "minimum": min(samples), "maximum": max(samples),
            "mean": statistics.fmean(samples)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path, help="local rifelove-cli.exe")
    parser.add_argument("--ffprobe", required=True, type=Path, help="local ffprobe.exe")
    parser.add_argument("--source", required=True, action="append", type=Path,
                        help="local input video; repeat for a content matrix")
    parser.add_argument("--model-dir", required=True, type=Path, help="verified local RIFE model pack")
    parser.add_argument("--runtime-dir", required=True, type=Path, help="local ONNX Runtime CUDA/TensorRT DLL directory")
    parser.add_argument("--fps", required=True, action="append", type=parse_fraction,
                        help="target FPS as integer or rational; repeat to compare rates")
    parser.add_argument("--output-dir", required=True, type=Path, help="new or empty directory for benchmark evidence")
    parser.add_argument("--runs", type=int, default=3, help="measured runs per source/rate (default: 3)")
    parser.add_argument("--warmups", type=int, default=1, help="unscored warm-up runs per source/rate (default: 1)")
    parser.add_argument("--encoder", default="auto", help="auto or an encoder name (default: auto)")
    parser.add_argument("--container", choices=("mkv", "mp4", "mov"), default="mkv")
    parser.add_argument("--bitrate", type=int, default=8_000_000, help="target encoder bitrate in bits/second")
    parser.add_argument("--pixel-format", default="", help="optional explicit output pixel format")
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=3600, help="maximum seconds per interpolation run")
    parser.add_argument("--keep-videos", action="store_true", help="retain successfully validated output videos")
    args = parser.parse_args()

    if not 1 <= args.runs <= 20 or not 0 <= args.warmups <= 10:
        parser.error("--runs must be 1..20 and --warmups must be 0..10")
    if args.gpu < 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--gpu must be nonnegative and --timeout must be positive")
    if args.bitrate <= 0:
        parser.error("--bitrate must be positive")
    for path, kind in ((args.cli, "file"), (args.ffprobe, "file"), (args.model_dir / "model.json", "file"),
                       (args.model_dir / "model.onnx", "file"), (args.runtime_dir / "onnxruntime.dll", "file")):
        if not path.is_file():
            parser.error(f"required local {kind} not found: {path}")
    if not args.runtime_dir.is_dir():
        parser.error(f"runtime directory not found: {args.runtime_dir}")
    for source in args.source:
        if not source.is_file():
            parser.error(f"source video not found: {source}")

    output_dir = args.output_dir.absolute()
    if output_dir.exists() and (output_dir.is_symlink() or not output_dir.is_dir() or any(output_dir.iterdir())):
        parser.error(f"output directory must be new or empty: {output_dir}")

    model = json.loads((args.model_dir / "model.json").read_text(encoding="utf-8"))
    actual_model_hash = digest_file(args.model_dir / "model.onnx")
    if model.get("sha256") != actual_model_hash:
        parser.error("local model graph does not match model.json SHA-256")
    cli_hash = digest_file(args.cli)
    ffprobe_version_result = run_text([str(args.ffprobe), "-version"], 15)
    if ffprobe_version_result.returncode or not ffprobe_version_result.stdout.splitlines():
        parser.error(f"local ffprobe version query failed: {ffprobe_version_result.stderr[-2000:]}")
    ffprobe_version = ffprobe_version_result.stdout.splitlines()[0]
    gpu_command = run_text([str(args.cli), "gpu-info", str(args.gpu)], 30)
    if gpu_command.returncode:
        parser.error(f"local CLI GPU query failed: {gpu_command.stderr[-2000:]}")
    gpu = json.loads(gpu_command.stdout)
    sources = [source_metadata(args.ffprobe, source.resolve()) for source in args.source]
    output_dir.mkdir(parents=True, exist_ok=True)
    artifacts_dir = output_dir / "artifacts"
    artifacts_dir.mkdir(exist_ok=True)
    summary_path = output_dir / "benchmark.json"

    document = {
        "schemaVersion": SCHEMA_VERSION,
        "status": "running",
        "method": {
            "measurement": "Validated end-to-end RifeLove CLI runs with unique outputs",
            "warmupsExcludedFromSummary": True,
            "sourceFilesHashedBeforeAndAfter": True,
            "outputFrameCountIndependentlyCheckedWithFfprobe": True,
            "perceptualQualityMeasured": False,
            "notes": ["Runs are ordered; CUDA/TensorRT caches may become warm after an earlier run.",
                      "Synthetic or ordinary sources establish performance only, not a quality ranking."],
        },
        "environment": {"os": platform.platform(), "python": sys.version.split()[0],
                         "cliPath": str(args.cli.resolve()), "cliSha256": cli_hash,
                         "ffprobePath": str(args.ffprobe.resolve()), "ffprobeVersion": ffprobe_version,
                         "modelId": model.get("id"), "modelVersion": model.get("version"),
                         "modelSha256": actual_model_hash, "runtimeDirectory": str(args.runtime_dir.resolve()),
                         "gpu": gpu},
        "configuration": {"targetRates": [str(rate) for rate in args.fps], "encoderRequested": args.encoder,
                           "container": args.container, "bitrate": args.bitrate,
                           "pixelFormat": args.pixel_format or None, "runs": args.runs,
                           "warmups": args.warmups, "gpuOrdinal": args.gpu},
        "sources": sources,
        "runs": [],
        "aggregates": [],
    }
    atomic_json(summary_path, document)
    failures = 0
    run_number = 0
    for source_index, source_info in enumerate(sources, start=1):
        source = Path(source_info["path"])
        base = safe_stem(source)
        for rate in args.fps:
            group_key = f"source-{source_index}:{rate}"
            for index in range(args.warmups + args.runs):
                run_number += 1
                kind = "warmup" if index < args.warmups else "measurement"
                measured_index = index - args.warmups + 1 if kind == "measurement" else index + 1
                rate_tag = str(rate).replace("/", "-")
                output = artifacts_dir / f"run-{run_number:03d}-{base}-{rate_tag}.{args.container}"
                report_path = output_dir / f"run-{run_number:03d}-report.json"
                command = [str(args.cli), "interpolate", str(source), str(output), "--fps", str(rate),
                           "--model-dir", str(args.model_dir), "--runtime-dir", str(args.runtime_dir),
                           "--encoder", args.encoder, "--bitrate", str(args.bitrate), "--report", str(report_path),
                           "--gpu", str(args.gpu)]
                if args.pixel_format:
                    command.extend(("--pixel-format", args.pixel_format))
                started = time.perf_counter()
                record = {"runNumber": run_number, "group": group_key, "kind": kind,
                          "measurementIndex": measured_index if kind == "measurement" else None,
                          "sourceIndex": source_index, "targetRate": str(rate),
                          "command": command, "status": "running"}
                try:
                    process = run_text(command, args.timeout)
                    record["wallSeconds"] = time.perf_counter() - started
                    record["returnCode"] = process.returncode
                    if process.stdout.strip():
                        record["stdoutLastLine"] = process.stdout.strip().splitlines()[-1]
                    if process.stderr.strip():
                        record["stderrTail"] = process.stderr[-3000:]
                    if process.returncode:
                        record["status"] = "failed"
                        record["error"] = process.stderr[-5000:]
                    else:
                        stdout_lines = process.stdout.strip().splitlines()
                        if not stdout_lines:
                            raise RuntimeError("CLI completed without a JSON result line")
                        cli_result = json.loads(stdout_lines[-1])
                        report = json.loads(report_path.read_text(encoding="utf-8"))
                        if cli_result.get("event") != "interpolation_completed":
                            raise RuntimeError("CLI did not return interpolation_completed")
                        performance = report["performance"]
                        frames = int(cli_result["output_frames"])
                        decoded = output_metadata(args.ffprobe, output, frames)
                        record.update({"status": "completed", "outputFrames": frames,
                                       "cliElapsedSeconds": cli_result["elapsed_seconds"],
                                       "cliPipelineFps": cli_result["processing_fps"],
                                       "endToEndFps": performance["endToEndOutputFramesPerSecond"],
                                       "pipelineFps": performance["pipelineOutputFramesPerSecond"],
                                       "actualEncoder": report["output"]["encoder"],
                                       "encoderFallback": report["encoding"].get("fallbackReason"),
                                       "inferenceBackend": report["model"].get("backend"),
                                       "inferenceFallback": report["model"].get("fallbackReason"),
                                       "stageTimings": performance["stageTimings"],
                                       "outputProbe": decoded,
                                       "outputBytes": output.stat().st_size,
                                       "reportPath": str(report_path)})
                        if not args.keep_videos:
                            output.unlink()
                            record["outputRetained"] = False
                        else:
                            record["outputRetained"] = True
                except subprocess.TimeoutExpired as error:
                    record.update({"status": "timeout", "wallSeconds": time.perf_counter() - started,
                                   "error": str(error)})
                except (OSError, RuntimeError, KeyError, ValueError, json.JSONDecodeError) as error:
                    record.update({"status": "failed", "wallSeconds": time.perf_counter() - started,
                                   "error": str(error)})
                if record["status"] in ("failed", "timeout"):
                    failures += 1
                    if output.exists():
                        record["failedOutputPath"] = str(output)
                    if report_path.exists():
                        record["reportPath"] = str(report_path)
                document["runs"].append(record)
                document["status"] = "running"
                atomic_json(summary_path, document)
                print(json.dumps({"event": "benchmark_run", "run": run_number, "group": group_key,
                                  "kind": kind, "status": record["status"],
                                  "endToEndFps": record.get("endToEndFps"), "error": record.get("error")},
                                 ensure_ascii=False), flush=True)

            samples = [item["endToEndFps"] for item in document["runs"]
                       if item["group"] == group_key and item["kind"] == "measurement" and item["status"] == "completed"]
            pipeline_samples = [item["pipelineFps"] for item in document["runs"]
                                if item["group"] == group_key and item["kind"] == "measurement" and item["status"] == "completed"]
            document["aggregates"].append({"group": group_key, "sourceIndex": source_index,
                                           "targetRate": str(rate), "completedMeasurements": len(samples),
                                           "endToEndFps": summarize(samples), "pipelineFps": summarize(pipeline_samples)})
            atomic_json(summary_path, document)

    unchanged = all(digest_file(Path(info["path"])) == info["sha256"] for info in sources)
    document["sourceFilesUnchanged"] = unchanged
    if not unchanged:
        failures += 1
        document["sourceIntegrityError"] = "At least one source changed while the benchmark was running."
    document["status"] = "completed" if failures == 0 else "completed_with_failures"
    document["finishedAtLocal"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    atomic_json(summary_path, document)
    print(json.dumps({"event": "benchmark_completed", "status": document["status"],
                      "summary": str(summary_path), "runs": len(document["runs"]),
                      "failedRuns": failures}, ensure_ascii=False))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

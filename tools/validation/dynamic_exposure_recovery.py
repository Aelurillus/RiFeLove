#!/usr/bin/env python3
"""Validate dynamic-exposure accounting and checkpoint recovery on a local GPU.

The harness uses only a fresh synthetic BT.709 fixture, private SQLite queues,
and local RIFE/runtime files. It checks that output-frame classification remains
an output count while the separately reported RIFE inference-call count grows
with multi-sample exposure synthesis.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import subprocess
import time


def run(*args: object, timeout: int = 300, binary: bool = False):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}): {result.stderr[-6000:].decode('utf-8', 'replace')}")
    return result.stdout if binary else result.stdout.decode("utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(cli: Path, database: Path):
    return [json.loads(line) for line in run(cli, "queue-list", database).splitlines() if line.strip()]


def frames(ffprobe: Path, path: Path):
    raw = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams", "-show_frames",
                         "-show_entries", "stream=time_base:frame=best_effort_timestamp", "-of", "json", path))
    streams = raw.get("streams", [])
    if len(streams) != 1:
        raise RuntimeError(f"Expected one video stream in {path}")
    base = Fraction(streams[0]["time_base"])
    values = [int(item["best_effort_timestamp"]) * base for item in raw.get("frames", [])]
    return values, base


def raw_video(ffmpeg: Path, path: Path) -> bytes:
    return run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-", binary=True)


def raw_audio(ffmpeg: Path, path: Path) -> bytes:
    return run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-f", "s16le", "-", binary=True)


def result_of(cli: Path, database: Path):
    values = rows(cli, database)
    if len(values) != 1 or values[0]["state"] != "completed":
        raise RuntimeError(f"Queue did not complete exactly one task: {values}")
    return values[0]["result"]


def inference_calls(result: dict) -> int:
    # Keep the harness strict once the core contract is present, while allowing
    # the temporary field aliases used by development builds during migration.
    keys = ("inferenceCalls", "rifeInferenceCalls", "modelInferenceCalls")
    present = [(key, result[key]) for key in keys if key in result]
    if len(present) != 1:
        raise RuntimeError("Pipeline result must expose exactly one independent inference-call counter")
    value = present[0][1]
    if not isinstance(value, int) or value < 0:
        raise RuntimeError(f"Invalid inference-call counter {present[0]}")
    return value


def attempt_counters(result: dict) -> tuple[int, int]:
    output = result["outputFrames"]
    processed = result.get("processedFrames")
    reused = result.get("reusedFrames")
    if processed is None and reused is None:
        # Results written by the pre-resume schema represent a non-resumed
        # attempt, so preserve their old interpretation for diagnostics.
        processed, reused = output, 0
    elif processed is None:
        processed = output - reused
    elif reused is None:
        reused = output - processed
    if not isinstance(processed, int) or not isinstance(reused, int) or processed < 0 or reused < 0 or processed + reused != output:
        raise RuntimeError(f"Invalid current-attempt frame counters: output={output}, processed={processed}, reused={reused}")
    elapsed = result.get("elapsedSeconds", 0.0)
    expected_speed = processed / elapsed if processed and isinstance(elapsed, (int, float)) and math.isfinite(elapsed) and elapsed > 0 else 0.0
    actual_speed = result.get("processingFps")
    if not isinstance(actual_speed, (int, float)) or not math.isfinite(actual_speed) or actual_speed < 0:
        raise RuntimeError(f"Invalid processing speed: {actual_speed}")
    if abs(actual_speed - expected_speed) > max(1e-9, expected_speed * 1e-6):
        raise RuntimeError(f"Processing speed uses the wrong frame count: actual={actual_speed}, expected={expected_speed}")
    return processed, reused


def verify_timestamps(ffprobe: Path, path: Path, count: int):
    values, base = frames(ffprobe, path)
    if len(values) != count:
        raise RuntimeError(f"{path} has {len(values)} frames, expected {count}")
    for index, value in enumerate(values):
        expected = Fraction(index, 60)
        if abs(value - expected) > base / 2:
            raise RuntimeError(f"{path} frame {index} PTS {value} differs from {expected}")
    return values, base


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("native", "cli", "ffmpeg", "ffprobe", "rife", "runtime", "workdir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.workdir.resolve()
    if root.exists():
        raise RuntimeError("Choose a fresh isolated directory; existing evidence is never overwritten")
    root.mkdir(parents=True)

    source = root / "source.mkv"
    blur_output = root / "dynamic-exposure.mkv"
    control_output = root / "no-exposure-control.mkv"
    run(args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
        "testsrc2=size=192x128:rate=30:duration=0.4", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=48000:duration=0.4", "-vf",
        "noise=alls=10:allf=t+u,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
        "-c:v", "ffv1", "-c:a", "flac", "-shortest", source)
    source_hash = digest(source)

    # Export and import a queue plan so motionBlur is exercised through the
    # persisted application settings path rather than a test-only API.
    seed = root / "seed.sqlite"
    run(args.cli, "queue-add", seed, source, blur_output, "--fps", "60", "--model-dir", args.rife,
        "--runtime-dir", args.runtime, "--backend", "cuda", "--encoder", "ffv1")
    plan = json.loads(run(args.cli, "queue-export", seed))
    if len(plan.get("jobs", [])) != 1:
        raise RuntimeError("Expected one queue plan")
    plan["jobs"][0]["configuration"]["motionBlur"] = {"enabled": True, "shutterAngleDegrees": 180, "sampleCount": 4}
    blur_plan = root / "blur-plan.json"
    blur_plan.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    baseline_db = root / "baseline.sqlite"
    run(args.cli, "queue-import", baseline_db, blur_plan)
    run(args.cli, "queue-run", baseline_db)
    baseline_result = result_of(args.cli, baseline_db)

    output_frames = baseline_result["outputFrames"]
    calls = inference_calls(baseline_result)
    baseline_processed, baseline_reused = attempt_counters(baseline_result)
    if baseline_processed != output_frames or baseline_reused != 0:
        raise RuntimeError("Uninterrupted baseline did not classify every output frame as current-attempt work")
    if baseline_result["aiFrames"] + baseline_result["originalFrames"] + baseline_result["heldFrames"] != output_frames:
        raise RuntimeError("Dynamic-exposure baseline frame classification does not cover output frames")
    if calls <= output_frames:
        raise RuntimeError(f"Four-sample dynamic exposure did not increase independent inference calls: calls={calls}, output={output_frames}")

    # A no-exposure control confirms the multi-sample path generated distinct
    # output content rather than merely changing the reported counters.
    control_seed = root / "control-seed.sqlite"
    control_db = root / "control.sqlite"
    run(args.cli, "queue-add", control_seed, source, control_output, "--fps", "60", "--model-dir", args.rife,
        "--runtime-dir", args.runtime, "--backend", "cuda", "--encoder", "ffv1")
    control_plan = json.loads(run(args.cli, "queue-export", control_seed))
    control_plan_path = root / "control-plan.json"
    control_plan_path.write_text(json.dumps(control_plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    run(args.cli, "queue-import", control_db, control_plan_path)
    run(args.cli, "queue-run", control_db)
    control_result = result_of(args.cli, control_db)
    if raw_video(args.ffmpeg, blur_output) == raw_video(args.ffmpeg, control_output):
        raise RuntimeError("Dynamic-exposure output is byte-identical to the no-exposure control")

    # The private native harness pauses after a committed two-frame segment,
    # then is terminated and resumed. It configures the same four-sample blur
    # settings directly in the developer-only hold path.
    recovery_root = root / "recovery"
    marker = recovery_root / "paused.txt"
    recovery_root.mkdir()
    command = [args.native, "motion-blur-hold", recovery_root, source, args.rife, args.runtime, marker]
    with (root / "holder.stdout.log").open("wb") as stdout, (root / "holder.stderr.log").open("wb") as stderr:
        holder = subprocess.Popen([str(item) for item in command], stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 300
            while not marker.exists():
                if holder.poll() is not None:
                    raise RuntimeError(f"Motion-blur hold worker exited before checkpoint: {holder.returncode}")
                if time.monotonic() > deadline:
                    raise TimeoutError("Motion-blur hold worker did not pause")
                time.sleep(0.2)
            database = recovery_root / "native-queue.sqlite"
            with sqlite3.connect(f"file:{database.as_posix()}?mode=ro", uri=True) as connection:
                manifest_row = connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()
                segment_count = connection.execute("SELECT COUNT(*) FROM task_checkpoint_segments").fetchone()[0]
            if manifest_row is None or segment_count < 1:
                raise RuntimeError("Motion-blur hold did not commit a checkpoint segment")
            holder.kill()
            holder.wait(timeout=15)
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.wait(timeout=15)
    run(args.native, "motion-blur-recover", recovery_root, source, args.rife, args.runtime, marker)
    recovered_result = result_of(args.cli, database)
    recovered_processed, recovered_reused = attempt_counters(recovered_result)
    recovered_output = recovery_root / "crash-output.mkv"
    recovered_calls = inference_calls(recovered_result)
    if recovered_result["aiFrames"] + recovered_result["originalFrames"] + recovered_result["heldFrames"] != recovered_result["outputFrames"]:
        raise RuntimeError("Recovered dynamic-exposure frame classification does not cover output frames")
    if recovered_calls <= recovered_result["outputFrames"]:
        raise RuntimeError("Recovered dynamic-exposure inference-call counter did not reflect multi-sample exposure")
    if recovered_result["outputFrames"] != output_frames:
        raise RuntimeError("Recovered output frame count differs from baseline")
    if recovered_reused <= 0 or recovered_processed >= recovered_result["outputFrames"]:
        raise RuntimeError("Checkpoint recovery did not report reused frames separately from current-attempt work")

    baseline_pts, baseline_base = verify_timestamps(args.ffprobe, blur_output, output_frames)
    recovered_pts, recovered_base = verify_timestamps(args.ffprobe, recovered_output, output_frames)
    for index, (actual, expected) in enumerate(zip(recovered_pts, baseline_pts)):
        if abs(actual - expected) > (recovered_base + baseline_base) / 2:
            raise RuntimeError(f"Recovered frame {index} PTS differs from baseline")
    baseline_video = raw_video(args.ffmpeg, blur_output)
    recovered_video = raw_video(args.ffmpeg, recovered_output)
    if recovered_video != baseline_video:
        raise RuntimeError("Recovered dynamic-exposure video differs from uninterrupted baseline")
    source_audio = raw_audio(args.ffmpeg, source)
    if not source_audio or raw_audio(args.ffmpeg, blur_output) != source_audio or raw_audio(args.ffmpeg, recovered_output) != source_audio:
        raise RuntimeError("Dynamic-exposure baseline or recovered audio differs from source")
    if digest(source) != source_hash:
        raise RuntimeError("Synthetic source changed during dynamic-exposure recovery")

    report = {
        "status": "passed",
        "source_sha256": source_hash,
        "output_frames": output_frames,
        "baseline_classification": {key: baseline_result[key] for key in ("aiFrames", "originalFrames", "heldFrames")},
        "baseline_inference_calls": calls,
        "baseline_processed_frames": baseline_processed,
        "baseline_reused_frames": baseline_reused,
        "baseline_processing_fps": baseline_result["processingFps"],
        "recovered_classification": {key: recovered_result[key] for key in ("aiFrames", "originalFrames", "heldFrames")},
        "recovered_inference_calls": recovered_calls,
        "recovered_processed_frames": recovered_processed,
        "recovered_reused_frames": recovered_reused,
        "recovered_processing_fps": recovered_result["processingFps"],
        "committed_segments_before_interrupt": segment_count,
        "video_sha256": hashlib.sha256(baseline_video).hexdigest(),
        "audio_sha256": hashlib.sha256(source_audio).hexdigest(),
        "control_video_sha256": hashlib.sha256(raw_video(args.ffmpeg, control_output)).hexdigest(),
        "fps": "60/1",
        "limitations": "Short synthetic BT.709 SDR fixture; validates dynamic-exposure accounting and checkpoint determinism, not perceptual quality or long-video stability.",
    }
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

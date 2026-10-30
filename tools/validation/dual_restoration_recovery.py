#!/usr/bin/env python3
"""Run an isolated dual-restoration checkpoint recovery against local GPU packs.

This developer-only harness creates a short tagged SDR fixture, runs an
uninterrupted dual-model baseline, then uses the native queue test harness to
pause after a committed checkpoint segment, terminates that private worker,
and resumes the task. It never opens the product queue or user media.
"""
from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time


def run(*args: object, timeout: int = 300, binary: bool = False):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=timeout)
    if result.returncode:
        stderr = result.stderr[-6000:].decode("utf-8", "replace")
        raise RuntimeError(f"{args[0]} failed ({result.returncode}): {stderr}")
    return result.stdout if binary else result.stdout.decode("utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def queue_rows(cli: Path, database: Path):
    return [json.loads(line) for line in run(cli, "queue-list", database).splitlines() if line.strip()]


def ffprobe_json(ffprobe: Path, *args: object, path: Path):
    return json.loads(run(ffprobe, "-v", "error", *args, path))


def timestamps(ffprobe: Path, path: Path):
    info = ffprobe_json(ffprobe, "-select_streams", "v:0", "-show_streams", "-show_frames",
                        "-show_entries", "stream=time_base:frame=best_effort_timestamp", "-of", "json", path=path)
    streams = info.get("streams", [])
    if len(streams) != 1:
        raise RuntimeError(f"Expected one video stream in {path}")
    time_base = Fraction(streams[0]["time_base"])
    frames = [int(frame["best_effort_timestamp"]) * time_base for frame in info.get("frames", [])]
    if not frames:
        raise RuntimeError(f"No decoded video timestamps in {path}")
    return frames, time_base


def decoded_video(ffmpeg: Path, path: Path) -> bytes:
    return run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0",
               "-pix_fmt", "yuv420p", "-f", "rawvideo", "-", binary=True)


def decoded_audio(ffmpeg: Path, path: Path) -> bytes:
    return run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:a:0",
               "-f", "s16le", "-", binary=True)


def verify_timestamps(ffprobe: Path, path: Path, expected_count: int, rate: Fraction):
    frames, time_base = timestamps(ffprobe, path)
    if len(frames) != expected_count:
        raise RuntimeError(f"{path} has {len(frames)} frames, expected {expected_count}")
    for index, actual in enumerate(frames):
        expected = Fraction(index, 1) / rate
        if abs(actual - expected) > time_base / 2:
            raise RuntimeError(f"{path} frame {index} PTS {actual} differs from {expected}")
    return frames, time_base


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("native", "cli", "ffmpeg", "ffprobe", "rife", "runtime", "temporal", "superres", "workdir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.workdir.resolve()
    if root.exists():
        raise RuntimeError("Choose a fresh isolated directory; existing evidence is never overwritten")
    root.mkdir(parents=True)

    source = root / "source.mkv"
    baseline = root / "baseline-dual.mkv"
    recovered = root / "recovery" / "crash-output.mkv"
    source_hash = None
    run(args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i",
        "testsrc2=size=192x128:rate=30:duration=0.4", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=48000:duration=0.4", "-vf",
        "noise=alls=10:allf=t+u,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
        "-c:v", "ffv1", "-c:a", "flac", "-shortest", source)
    source_hash = digest(source)

    # Build a dual-model queue plan using the same import path as the product.
    seed = root / "seed.sqlite"
    dual_baseline_db = root / "dual-baseline.sqlite"
    run(args.cli, "queue-add", seed, source, baseline, "--fps", "60", "--model-dir", args.rife,
        "--runtime-dir", args.runtime, "--restoration-model-dir", args.superres,
        "--backend", "cuda", "--encoder", "ffv1")
    plan = json.loads(run(args.cli, "queue-export", seed))
    if len(plan.get("jobs", [])) != 1:
        raise RuntimeError("Expected one baseline queue job")
    configuration = plan["jobs"][0]["configuration"]
    configuration["temporalRestorationModelDirectory"] = str(args.temporal.resolve())
    configuration["expectedTemporalRestorationDescriptorSha256"] = digest(args.temporal / "model.json")
    dual_plan = root / "dual-plan.json"
    dual_plan.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    run(args.cli, "queue-import", dual_baseline_db, dual_plan)
    run(args.cli, "queue-run", dual_baseline_db)
    rows = queue_rows(args.cli, dual_baseline_db)
    if len(rows) != 1 or rows[0]["state"] != "completed":
        raise RuntimeError(f"Uninterrupted dual baseline failed: {rows}")
    baseline_result = rows[0]["result"]
    expected_temporal = "fastdvdnet-clipped-five-frame-sdr8-eval"
    expected_superres = "realesrgan-x2plus-sdr8-local"
    if (baseline_result["temporalRestorationModelId"] != expected_temporal or
            baseline_result["restorationModelId"] != expected_superres):
        raise RuntimeError("Baseline result does not identify both restoration models")

    # Run the private integration harness with a two-frame checkpoint segment.
    recovery_root = root / "recovery"
    marker = recovery_root / "paused.txt"
    recovery_root.mkdir()
    hold = [args.native, "dual-restoration-hold", recovery_root, source, args.rife, args.runtime,
            marker, args.superres, args.temporal]
    with (root / "holder.stdout.log").open("wb") as stdout, (root / "holder.stderr.log").open("wb") as stderr:
        holder = subprocess.Popen([str(item) for item in hold], stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 300
            while not marker.exists():
                if holder.poll() is not None:
                    raise RuntimeError(f"Dual hold worker exited before checkpoint: {holder.returncode}")
                if time.monotonic() > deadline:
                    raise TimeoutError("Dual hold worker did not pause at a committed checkpoint")
                time.sleep(0.2)
            database = recovery_root / "native-queue.sqlite"
            with sqlite3.connect(f"file:{database.as_posix()}?mode=ro", uri=True) as connection:
                manifest_row = connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()
                segment_count = connection.execute("SELECT COUNT(*) FROM task_checkpoint_segments").fetchone()[0]
            if manifest_row is None or segment_count < 1:
                raise RuntimeError("Dual hold did not commit a checkpoint segment")
            manifest = json.loads(manifest_row[0])
            temporal_runtime = manifest.get("temporalRestorationRuntime")
            superres_runtime = manifest.get("restorationRuntime")
            if (temporal_runtime is None or superres_runtime is None or
                    temporal_runtime.get("modelId") != expected_temporal or
                    superres_runtime.get("modelId") != expected_superres or
                    not temporal_runtime.get("signature") or not superres_runtime.get("signature")):
                raise RuntimeError("Checkpoint does not bind both restoration runtime identities")
            holder.kill()
            holder.wait(timeout=15)
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.wait(timeout=15)

    run(args.native, "dual-restoration-recover", recovery_root, source, args.rife, args.runtime, marker)
    recovered_rows = queue_rows(args.cli, recovery_root / "native-queue.sqlite")
    if len(recovered_rows) != 1 or recovered_rows[0]["state"] != "completed":
        raise RuntimeError(f"Recovered dual task did not complete: {recovered_rows}")
    recovered_result = recovered_rows[0]["result"]
    if (recovered_result["temporalRestorationModelId"] != expected_temporal or
            recovered_result["restorationModelId"] != expected_superres):
        raise RuntimeError("Recovered result does not identify both restoration models")
    if recovered_result["outputFrames"] != baseline_result["outputFrames"]:
        raise RuntimeError("Recovered frame count differs from the uninterrupted dual baseline")

    baseline_frames, baseline_base = verify_timestamps(args.ffprobe, baseline, baseline_result["outputFrames"], Fraction(60))
    recovered_frames, recovered_base = verify_timestamps(args.ffprobe, recovered, baseline_result["outputFrames"], Fraction(60))
    for index, (actual, expected) in enumerate(zip(recovered_frames, baseline_frames)):
        if abs(actual - expected) > (recovered_base + baseline_base) / 2:
            raise RuntimeError(f"Recovered frame {index} PTS differs from the uninterrupted baseline")

    baseline_info = ffprobe_json(args.ffprobe, "-count_frames", "-show_streams", "-of", "json", path=baseline)
    recovered_info = ffprobe_json(args.ffprobe, "-count_frames", "-show_streams", "-of", "json", path=recovered)
    for label, info in (("baseline", baseline_info), ("recovered", recovered_info)):
        videos = [stream for stream in info.get("streams", []) if stream.get("codec_type") == "video"]
        if len(videos) != 1 or (videos[0].get("width"), videos[0].get("height")) != (384, 256):
            raise RuntimeError(f"{label} output does not have the expected 2x dimensions")
        if videos[0].get("r_frame_rate") != "60/1":
            raise RuntimeError(f"{label} output does not declare 60 FPS")

    source_audio = decoded_audio(args.ffmpeg, source)
    baseline_audio = decoded_audio(args.ffmpeg, baseline)
    recovered_audio = decoded_audio(args.ffmpeg, recovered)
    if not source_audio or baseline_audio != source_audio or recovered_audio != source_audio:
        raise RuntimeError("Dual baseline or recovered output did not preserve decoded source audio")

    baseline_video = decoded_video(args.ffmpeg, baseline)
    recovered_video = decoded_video(args.ffmpeg, recovered)
    if recovered_video != baseline_video:
        raise RuntimeError("Recovered dual output video bytes differ from the uninterrupted baseline")
    if digest(source) != source_hash:
        raise RuntimeError("Synthetic source changed during dual recovery")

    report = {
        "status": "passed",
        "source_sha256": source_hash,
        "output_frames": baseline_result["outputFrames"],
        "dimensions": [384, 256],
        "fps": "60/1",
        "committed_segments_before_interrupt": segment_count,
        "temporal_model": expected_temporal,
        "superres_model": expected_superres,
        "temporal_runtime": temporal_runtime,
        "superres_runtime": superres_runtime,
        "baseline_video_sha256": hashlib.sha256(baseline_video).hexdigest(),
        "recovered_video_sha256": hashlib.sha256(recovered_video).hexdigest(),
        "audio_sha256": hashlib.sha256(recovered_audio).hexdigest(),
        "pts_frames_compared": len(recovered_frames),
        "limitations": "Short synthetic BT.709 SDR fixture; validates wiring, identity, timing and recovery, not perceptual quality or long-video stability.",
    }
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

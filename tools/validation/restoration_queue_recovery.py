#!/usr/bin/env python3
"""Interrupt and resume a real local restoration queue on a short fixture."""

import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, timeout=120):
    result = subprocess.run([str(item) for item in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{args[0]} exited {result.returncode}: {result.stderr[-4000:].decode('utf-8', errors='replace')}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    for name in ("native", "cli", "ffmpeg", "ffprobe", "source", "rife", "runtime", "restoration", "reference", "workdir"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    root = args.workdir.resolve()
    if root.exists():
        raise SystemExit("Refusing to overwrite an existing work directory")
    root.mkdir(parents=True)
    marker = root / "paused.txt"
    database = root / "native-queue.sqlite"
    source_hash = digest(args.source)
    command = [str(args.native), "restoration-hold", str(root), str(args.source), str(args.rife),
               str(args.runtime), str(marker), str(args.restoration)]
    with (root / "holder.stdout.log").open("wb") as stdout, (root / "holder.stderr.log").open("wb") as stderr:
        holder = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 120
            while not marker.exists():
                if holder.poll() is not None:
                    raise RuntimeError(f"Worker exited before its checkpoint: {holder.returncode}")
                if time.monotonic() > deadline:
                    raise TimeoutError("Worker did not pause at a committed segment")
                time.sleep(0.2)
            with sqlite3.connect(f"file:{database.as_posix()}?mode=ro", uri=True) as connection:
                manifest = json.loads(connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0])
                segments = connection.execute("SELECT COUNT(*) FROM task_checkpoint_segments").fetchone()[0]
            runtime = manifest["restorationRuntime"]
            if segments < 1 or runtime["backend"] != "onnxruntime_cuda" or not runtime["signature"]:
                raise RuntimeError("Checkpoint lacks a committed segment or restoration runtime identity")
            holder.kill()
            holder.wait(timeout=10)
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.wait(timeout=10)

    recovery = subprocess.run([str(args.native), "restoration-recover", str(root), str(args.source),
                               str(args.rife), str(args.runtime), str(marker)], capture_output=True, timeout=120)
    (root / "recovery.stdout.log").write_bytes(recovery.stdout)
    (root / "recovery.stderr.log").write_bytes(recovery.stderr)
    if recovery.returncode:
        raise RuntimeError(f"Recovery failed ({recovery.returncode}): {recovery.stderr[-4000:].decode('utf-8', errors='replace')}")
    rows = [json.loads(line) for line in run(args.cli, "queue-list", database).decode("utf-8").splitlines()]
    if len(rows) != 1 or rows[0]["state"] != "completed":
        raise RuntimeError(f"Recovered task did not complete: {rows}")
    result = rows[0]["result"]
    if result["aiFrames"] + result["originalFrames"] + result["heldFrames"] != result["outputFrames"]:
        raise RuntimeError("Recovered task frame classification does not cover the complete output")
    output = root / "crash-output.mkv"
    info = json.loads(run(args.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams", "-show_frames",
                          "-show_entries", "stream=time_base:frame=best_effort_timestamp", "-of", "json", output))
    frames = info["frames"]
    time_base = Fraction(info["streams"][0]["time_base"])
    if len(frames) != 6 or any(abs(int(frame["best_effort_timestamp"]) * time_base - Fraction(i, 60)) > time_base / 2
                               for i, frame in enumerate(frames)):
        raise RuntimeError("Recovered frame count or timestamps differ from the 60 FPS fixture")
    def decoded(path):
        return run(args.ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-")
    actual, reference = decoded(output), decoded(args.reference)
    if len(actual) != len(reference):
        raise RuntimeError("Resumed decoded frame byte count differs from the uninterrupted reference")
    differences = [abs(a - b) for a, b in zip(actual, reference)]
    maximum = max(differences, default=0)
    changed_fraction = sum(value != 0 for value in differences) / len(differences)
    mse = sum(value * value for value in differences) / len(differences)
    psnr = 999.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    if maximum > 1 or changed_fraction > 0.0001 or psnr < 90:
        raise RuntimeError(f"Resumed frames exceed the 1-level/0.01%/90 dB comparison gate: max={maximum}, changed={changed_fraction}, PSNR={psnr}")
    if digest(args.source) != source_hash:
        raise RuntimeError("Source fixture changed during recovery")
    report = {"status": "passed", "frames": len(frames), "committed_segments_before_interrupt": segments,
              "restoration_runtime": runtime, "source_sha256": source_hash,
              "frame_classification": {"ai": result["aiFrames"], "original": result["originalFrames"], "held": result["heldFrames"]},
              "decoded_comparison": {"max_abs_8bit": maximum, "changed_fraction": changed_fraction, "psnr_db": psnr,
                                     "gate": "max 1 level, at most 0.01% changed, PSNR at least 90 dB"},
              "decoded_sha256": hashlib.sha256(actual).hexdigest()}
    (root / "recovery-check.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise both local restoration stages using an isolated synthetic queue.

This developer-only validation never reads user video or touches the product's
queue. It uses the existing queue import interface and does not ship with the
desktop application.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess


def run(*args: object, timeout: int = 240, binary: bool = False):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode}): {result.stderr[-5000:].decode('utf-8', 'replace')}")
    return result.stdout if binary else result.stdout.decode("utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "ffmpeg", "ffprobe", "rife", "runtime", "temporal", "superres", "workdir"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    root = args.workdir.resolve()
    if root.exists():
        raise RuntimeError("Choose a fresh isolated directory; existing evidence is never overwritten")
    root.mkdir(parents=True)
    source, baseline, output = (root / name for name in ("source.mkv", "superres-only.mkv", "dual.mkv"))
    run(args.ffmpeg, "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=size=192x128:rate=30:duration=0.1",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.1",
        "-vf", "noise=alls=10:allf=t+u,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
        "-c:v", "ffv1", "-c:a", "flac", source)
    source_hash = digest(source)
    seed, dual = root / "baseline.sqlite", root / "dual.sqlite"
    run(args.cli, "queue-add", seed, source, baseline, "--fps", "60", "--model-dir", args.rife,
        "--runtime-dir", args.runtime, "--restoration-model-dir", args.superres, "--backend", "auto", "--encoder", "ffv1")
    plan = json.loads(run(args.cli, "queue-export", seed))
    if len(plan["jobs"]) != 1:
        raise RuntimeError("Expected one isolated baseline task")
    job = plan["jobs"][0]
    job["output"] = str(output)
    configuration = job["configuration"]
    configuration["temporalRestorationModelDirectory"] = str(args.temporal.resolve())
    configuration["expectedTemporalRestorationDescriptorSha256"] = digest(args.temporal / "model.json")
    plan_path = root / "dual-plan.json"
    plan_path.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    run(args.cli, "queue-import", dual, plan_path)
    run(args.cli, "queue-run", dual)
    run(args.cli, "queue-run", seed)
    rows = [json.loads(line) for line in run(args.cli, "queue-list", dual).splitlines()]
    if len(rows) != 1 or rows[0]["state"] != "completed":
        raise RuntimeError(f"Dual task did not complete: {rows}")
    result = rows[0]["result"]
    if (result["restorationModelId"] != "realesrgan-x2plus-sdr8-local" or
            result["temporalRestorationModelId"] != "fastdvdnet-clipped-five-frame-sdr8-eval"):
        raise RuntimeError("The queue report does not identify both selected models")
    with sqlite3.connect(f"file:{dual.as_posix()}?mode=ro", uri=True) as connection:
        manifest = json.loads(connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0])
    if (manifest["restorationRuntime"]["modelId"] != result["restorationModelId"] or
            manifest["temporalRestorationRuntime"]["modelId"] != result["temporalRestorationModelId"]):
        raise RuntimeError("Checkpoint does not bind both restoration runtime identities")
    info = json.loads(run(args.ffprobe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", output))
    videos = [stream for stream in info["streams"] if stream["codec_type"] == "video"]
    if len(videos) != 1 or (videos[0]["width"], videos[0]["height"]) != (384, 256):
        raise RuntimeError("Dual output does not have the expected x2 dimensions")
    if videos[0]["r_frame_rate"] != "60/1" or int(videos[0]["nb_read_frames"]) != result["outputFrames"]:
        raise RuntimeError("Dual output FPS or decoded frame count differs from its queue result")
    def audio(path: Path) -> bytes:
        return run(args.ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-f", "s16le", "-", binary=True)
    original_audio, actual_audio = audio(source), audio(output)
    if not original_audio or original_audio != actual_audio:
        raise RuntimeError("Dual output did not preserve source audio samples")
    def frames(path: Path) -> bytes:
        return run(args.ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-", binary=True)
    baseline_frames, actual_frames = frames(baseline), frames(output)
    if len(actual_frames) != len(baseline_frames) or actual_frames == baseline_frames:
        raise RuntimeError("Dual output is missing frames or is identical to the super-resolution-only control")
    if digest(source) != source_hash:
        raise RuntimeError("The synthetic input changed during processing")
    report = {"status": "passed", "source_sha256": source_hash, "frames": result["outputFrames"],
              "dimensions": [384, 256], "fps": "60/1", "rife_backend": result["backend"],
              "temporal_model": result["temporalRestorationModelId"], "temporal_backend": result["temporalRestorationBackend"],
              "superres_model": result["restorationModelId"], "superres_backend": result["restorationBackend"],
              "decoded_sha256": hashlib.sha256(actual_frames).hexdigest(),
              "audio_sha256": hashlib.sha256(actual_audio).hexdigest(),
              "limitations": "One short synthetic SDR fixture; does not establish visual quality or long-video reliability."}
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

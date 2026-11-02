#!/usr/bin/env python3
"""Qualify isolated NAFNet -> Real-ESRGAN CUDA processing on local hardware."""

from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time


def run(*arguments: object, timeout: int = 300) -> bytes:
    command = [str(value) for value in arguments]
    result = subprocess.run(command, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited {result.returncode}: "
                           + result.stderr[-3000:].decode("utf-8", "replace"))
    return result.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def probe(ffprobe: Path, path: Path) -> tuple[dict, list[Fraction], Fraction]:
    info = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0",
                          "-count_frames", "-show_streams", "-show_frames",
                          "-show_entries",
                          "stream=width,height,r_frame_rate,nb_read_frames,time_base:"
                          "frame=best_effort_timestamp", "-of", "json", path))
    stream = info["streams"][0]
    tick = Fraction(stream["time_base"])
    pts = [int(frame["best_effort_timestamp"]) * tick for frame in info["frames"]]
    return stream, pts, tick


def gpu_sample() -> tuple[int, int, int]:
    output = run("nvidia-smi", "--query-gpu=memory.used,memory.free,utilization.gpu",
                 "--format=csv,noheader,nounits", timeout=15).decode().strip().splitlines()
    if len(output) != 1:
        raise RuntimeError("Expected exactly one NVIDIA GPU for the memory sample")
    return tuple(int(value.strip()) for value in output[0].split(","))  # type: ignore[return-value]


def gpu_name() -> str:
    names = run("nvidia-smi", "--query-gpu=name", "--format=csv,noheader", timeout=15).decode().strip().splitlines()
    if len(names) != 1:
        raise RuntimeError("Expected exactly one NVIDIA GPU for this qualification")
    return names[0].strip()


def execute(runner: Path, mode: str, root: Path, source: Path, rife: Path,
            runtime: Path, deblur: Path, superres: Path) -> tuple[dict, dict]:
    stdout_path = root / f"{mode}.stdout.json"
    stderr_path = root / f"{mode}.stderr.log"
    command = [str(item) for item in
               (runner, mode, root, source, rife, runtime, deblur, superres)]
    baseline = gpu_sample()
    peak_used, minimum_free = baseline[:2]
    peak_utilization = baseline[2]
    samples = 1
    started = time.monotonic()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            while process.poll() is None:
                time.sleep(0.25)
                used, free, utilization = gpu_sample()
                peak_used = max(peak_used, used)
                minimum_free = min(minimum_free, free)
                peak_utilization = max(peak_utilization, utilization)
                samples += 1
                if time.monotonic() - started > 300:
                    raise TimeoutError(f"{mode} exceeded 300 seconds")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)
    wall = time.monotonic() - started
    if process.returncode:
        raise RuntimeError(f"{mode} failed ({process.returncode}): "
                           + stderr_path.read_text(errors="replace")[-3000:])
    result = json.loads(stdout_path.read_text(encoding="utf-8").splitlines()[-1])
    return result, {
        "wall_seconds": wall,
        "baseline_gpu_used_mib": baseline[0],
        "peak_total_gpu_used_mib": peak_used,
        "minimum_gpu_free_mib": minimum_free,
        "peak_gpu_utilization_percent": peak_utilization,
        "sample_count": samples,
    }


def decoded_rgb(ffmpeg: Path, path: Path) -> bytes:
    return run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0",
               "-pix_fmt", "rgb24", "-f", "rawvideo", "-")


def direct_cuda_probe(validator: Path, pack: Path, runtime: Path,
                      input_rgb: Path, width: int, height: int,
                      output_rgb: Path) -> dict:
    command = [validator, pack, runtime, input_rgb, width, height, output_rgb, "cuda"]
    if "nafnet" in pack.name:
        command.append(0.2)
    result = json.loads(run(*command, timeout=180))
    if result["backend"] != "onnxruntime_cuda" or result["provider_events"].get(
            "CUDAExecutionProvider", 0) <= 0:
        raise RuntimeError(f"{pack.name} did not execute CUDA graph nodes: {result}")
    return {
        "backend": result["backend"],
        "provider_events": result["provider_events"],
        "elapsed_seconds": result["elapsed_seconds"],
        "output_dimensions": [result["width"], result["height"]],
        "profile": result["profile"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "ffmpeg", "ffprobe", "validator", "source",
                 "rife", "runtime", "deblur", "superres", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    if root.exists():
        raise RuntimeError("Use a fresh isolated output directory; evidence is never overwritten")
    root.mkdir(parents=True)
    source = args.source.resolve()
    source_hash = sha256(source)
    input_stream, input_pts, _ = probe(args.ffprobe, source)
    width, height = input_stream["width"], input_stream["height"]
    if input_stream["r_frame_rate"] != "30/1" or len(input_pts) != 8:
        raise RuntimeError("This qualification fixture must be eight 30 FPS source frames")
    deblur_model = json.loads((args.deblur / "model.json").read_text(encoding="utf-8"))
    superres_model = json.loads((args.superres / "model.json").read_text(encoding="utf-8"))
    print("Running isolated dual-model CUDA pipeline", flush=True)
    dual, dual_gpu = execute(args.runner, "dual-reference", root, source, args.rife,
                             args.runtime, args.deblur, args.superres)
    if (dual["outputFrames"] != 16 or dual["validation"]["decodedFrames"] != 16
            or dual["deblurRestorationModelId"] != deblur_model["id"]
            or dual["restorationModelId"] != superres_model["id"]
            or dual["deblurRestorationBackend"] != "onnxruntime_cuda"
            or dual["restorationBackend"] != "onnxruntime_cuda"
            or dual["restorationScale"] != {"numerator": 2, "denominator": 1}
            or dual["automaticDetailRestoration"]["conditionedOutputFrames"] <= 0):
        raise RuntimeError(f"Dual-model execution identity is incomplete: {dual}")
    with sqlite3.connect(f"file:{(root / 'dual.sqlite').as_posix()}?mode=ro",
                         uri=True) as database:
        manifest = json.loads(database.execute(
            "SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0])
    if (manifest["job"]["schemaVersion"] < 14
            or manifest["outputImageOrder"] != "deblur-before-super-resolution-v1"
            or manifest["deblurRuntime"]["modelId"] != deblur_model["id"]
            or manifest["restorationRuntime"]["modelId"] != superres_model["id"]
            or manifest["deblurRuntime"]["descriptorSha256"] != sha256(args.deblur / "model.json")
            or manifest["job"]["expectedRestorationDescriptorSha256"]
            != sha256(args.superres / "model.json")):
        raise RuntimeError("Checkpoint failed to pin the two model identities and output order")
    print("Tracing every direct restoration call", flush=True)
    direct, direct_gpu = execute(args.runner, "dual-direct", root, source, args.rife,
                                 args.runtime, args.deblur, args.superres)
    if (direct["outputFrames"] != 16
            or direct["deblurRestorationModelId"] != deblur_model["id"]
            or direct["restorationModelId"] != superres_model["id"]):
        raise RuntimeError("Direct dual-model execution has inconsistent model identity")
    trace = [line.split("\t") for line in (root / "direct-stage-order.tsv").read_text().splitlines()]
    if len(trace) != 32:
        raise RuntimeError(f"Expected two restoration stage events per output frame, found {len(trace)}")
    for index in range(16):
        expected = Fraction(index, 60)
        first, second = trace[2 * index:2 * index + 2]
        if (len(first) != 2 or len(second) != 2
                or Fraction(first[0]) != expected or Fraction(second[0]) != expected
                or first[1] != deblur_model["id"] or second[1] != superres_model["id"]):
            raise RuntimeError(f"Restoration order or exact output PTS changed at frame {index}")
    output_stream, output_pts, output_tick = probe(args.ffprobe, root / "dual.mkv")
    if ((output_stream["width"], output_stream["height"], output_stream["r_frame_rate"],
         int(output_stream["nb_read_frames"])) != (width * 2, height * 2, "60/1", 16)
            or len(output_pts) != 16 or Fraction(dual["endExclusive"]) != Fraction(8, 30)
            or any(abs(actual - Fraction(i, 60)) > output_tick / 2
                   for i, actual in enumerate(output_pts))):
        raise RuntimeError("Dual output size, frame count, or quantized PTS is invalid")
    direct_stream, direct_pts, direct_tick = probe(args.ffprobe, root / "dual-direct.mkv")
    if ((direct_stream["width"], direct_stream["height"], direct_stream["r_frame_rate"],
         int(direct_stream["nb_read_frames"])) != (width * 2, height * 2, "60/1", 16)
            or len(direct_pts) != 16
            or any(abs(actual - Fraction(i, 60)) > direct_tick / 2
                   for i, actual in enumerate(direct_pts))):
        raise RuntimeError("Direct dual output size, frame count, or quantized PTS is invalid")
    print("Running super-resolution-only control", flush=True)
    control, control_gpu = execute(args.runner, "superres-reference", root, source,
                                   args.rife, args.runtime, args.deblur, args.superres)
    if (control["restorationModelId"] != superres_model["id"]
            or control["deblurRestorationModelId"] or control["outputFrames"] != 16):
        raise RuntimeError("Super-resolution-only control has unexpected model identity")
    dual_pixels = decoded_rgb(args.ffmpeg, root / "dual.mkv")
    direct_pixels = decoded_rgb(args.ffmpeg, root / "dual-direct.mkv")
    control_pixels = decoded_rgb(args.ffmpeg, root / "superres.mkv")
    frame_bytes = width * height * 4 * 3
    if (len(dual_pixels) != len(control_pixels) or len(dual_pixels) != len(direct_pixels)
            or len(dual_pixels) != frame_bytes * 16):
        raise RuntimeError("Decoded dual, direct, and control outputs have inconsistent dimensions")
    changed_frames = [i for i in range(16)
                      if dual_pixels[i * frame_bytes:(i + 1) * frame_bytes]
                      != control_pixels[i * frame_bytes:(i + 1) * frame_bytes]]
    if not any(index < 8 for index in changed_frames):
        raise RuntimeError("NAFNet-conditioned frames did not change the final x2 output")
    input_rgb = root / "first-source.rgb"
    input_rgb.write_bytes(decoded_rgb(args.ffmpeg, source)[:width * height * 3])
    print("Profiling CUDA graph execution for both model packs", flush=True)
    deblur_profile = direct_cuda_probe(args.validator, args.deblur, args.runtime,
                                       input_rgb, width, height, root / "nafnet-direct.rgb")
    superres_profile = direct_cuda_probe(args.validator, args.superres, args.runtime,
                                         input_rgb, width, height, root / "esrgan-direct.rgb")
    if sha256(source) != source_hash:
        raise RuntimeError("Synthetic source changed during dual-model qualification")
    report = {
        "status": "passed",
        "source_sha256": source_hash,
        "gpu": gpu_name(),
        "input_dimensions": [width, height],
        "output_dimensions": [width * 2, height * 2],
        "output_frames": 16,
        "output_rate": "60/1",
        "output_end_exclusive": dual["endExclusive"],
        "stage_order": "NAFNet deblur then Real-ESRGAN x2 on each output frame",
        "stage_events_verified": len(trace),
        "deblur_model": deblur_model["id"],
        "superres_model": superres_model["id"],
        "conditioned_output_frames": dual["automaticDetailRestoration"]["conditionedOutputFrames"],
        "changed_frames_against_superres_only": changed_frames,
        "dual_decoded_rgb_sha256": hashlib.sha256(dual_pixels).hexdigest(),
        "direct_decoded_rgb_sha256": hashlib.sha256(direct_pixels).hexdigest(),
        "control_decoded_rgb_sha256": hashlib.sha256(control_pixels).hexdigest(),
        "dual_gpu": dual_gpu,
        "direct_gpu": direct_gpu,
        "control_gpu": control_gpu,
        "dual_pipeline_seconds": dual["elapsedSeconds"],
        "direct_pipeline_seconds": direct["elapsedSeconds"],
        "dual_restoration_seconds": dual["timings"]["restorationSeconds"],
        "deblur_direct_cuda": deblur_profile,
        "superres_direct_cuda": superres_profile,
        "limitations": "One synthetic SDR clip on one local NVIDIA GPU. Sampled total GPU memory "
                       "includes the desktop and other processes; this is not a perceptual-quality "
                       "or long-video benchmark.",
    }
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()

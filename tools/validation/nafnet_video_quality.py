"""Measure a local image-restoration candidate on a short recorded-video proxy.

Each source frame is its own decoded RGB reference, not an uncompressed or
high-frame-rate ground truth. A horizontal box blur is an artificial damage
model, not proof of performance on real motion blur. No media is uploaded.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verified_graph(pack: Path) -> tuple[Path, str]:
    """Bind the reported graph identity to the descriptor used by the validator."""
    descriptor = json.loads((pack / "model.json").read_text(encoding="utf-8"))
    if descriptor.get("modelFile") != "model.onnx":
        raise ValueError("This evaluator requires a pack whose modelFile is model.onnx")
    graph = pack / "model.onnx"
    digest = sha256(graph)
    if descriptor.get("sha256", "").lower() != digest:
        raise ValueError("Model graph SHA-256 does not match the pack descriptor")
    return graph, digest


def horizontal_box_blur(frames: np.ndarray, width: int) -> np.ndarray:
    if frames.ndim != 4 or frames.shape[-1] != 3 or frames.dtype != np.uint8:
        raise ValueError("Expected uint8 NHWC RGB frames")
    if width < 1 or width > 63 or width % 2 == 0:
        raise ValueError("Blur width must be odd and within 1..63")
    padded = np.pad(frames.astype(np.float32), ((0, 0), (0, 0), (width // 2, width // 2), (0, 0)), mode="edge")
    acc = np.zeros_like(frames, dtype=np.float32)
    for offset in range(width):
        acc += padded[:, :, offset:offset + frames.shape[2], :]
    return np.rint(acc / width).clip(0, 255).astype(np.uint8)


def metrics(frames: np.ndarray, reference: np.ndarray) -> dict:
    if frames.shape != reference.shape or frames.ndim != 4:
        raise ValueError("Reference and output dimensions differ")
    error = frames.astype(np.float64) - reference.astype(np.float64)
    mse = float(np.mean(error * error))
    temporal = float(np.sqrt(np.mean(np.diff(error, axis=0) ** 2))) if len(error) > 1 else None
    reference_dx = np.diff(reference.astype(np.float64), axis=2)
    output_dx = np.diff(frames.astype(np.float64), axis=2)
    return {"psnr_db": None if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse)),
            "rmse_levels": float(np.sqrt(mse)),
            "temporal_error_difference_rmse_levels": temporal,
            "horizontal_gradient_error_levels": float(np.mean(np.abs(output_dx - reference_dx)))}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("video", "pack", "runtime", "validator", "ffmpeg", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--frames", type=int, default=3)
    parser.add_argument("--width", type=int, default=384)
    parser.add_argument("--height", type=int, default=288)
    parser.add_argument("--blur-width", type=int, default=9)
    parser.add_argument("--provider", choices=("cuda", "tensorrt"), default="cuda")
    args = parser.parse_args()
    if not 2 <= args.frames <= 32 or not 64 <= args.width <= 768 or not 64 <= args.height <= 768:
        raise ValueError("Expected 2..32 frames and dimensions within 64..768")
    if args.output.exists():
        raise ValueError("Choose a fresh output directory; evidence is never overwritten")
    _, graph_digest = verified_graph(args.pack)
    args.output.mkdir(parents=True)
    command = [str(args.ffmpeg.resolve()), "-nostdin", "-v", "error", "-i", str(args.video.resolve()),
               "-frames:v", str(args.frames), "-vf", f"scale={args.width}:{args.height}:flags=lanczos,format=rgb24",
               "-f", "rawvideo", "-"]
    decoded = subprocess.run(command, check=True, capture_output=True).stdout
    expected = args.frames * args.height * args.width * 3
    if len(decoded) != expected:
        raise ValueError(f"Decoded {len(decoded)} bytes; expected exactly {expected}")
    reference = np.frombuffer(decoded, np.uint8).reshape(args.frames, args.height, args.width, 3).copy()
    blurred = horizontal_box_blur(reference, args.blur_width)
    report = {"schemaVersion": 1, "sourceSha256": sha256(args.video),
              "graphSha256": graph_digest, "validatorSha256": sha256(args.validator),
              "frames": args.frames, "width": args.width, "height": args.height,
              "blurWidthPixels": args.blur_width, "providerRequested": args.provider,
              "limitations": "Decoded RGB proxy, artificial horizontal blur and short clip; no true camera reference, optical-flow flicker assessment, HDR or release-quality claim.",
              "cases": {}}
    for name, sequence in (("clean", reference), ("blurred", blurred)):
        outputs = []
        records = []
        for index, frame in enumerate(sequence):
            source = args.output / f"{name}-{index:02d}-input.rgb"
            target = args.output / f"{name}-{index:02d}-output.rgb"
            source.write_bytes(frame.tobytes())
            started = time.monotonic()
            run = subprocess.run([str(args.validator.resolve()), str(args.pack.resolve()), str(args.runtime.resolve()),
                                  str(source.resolve()), str(args.width), str(args.height), str(target.resolve()), args.provider],
                                 capture_output=True, text=True)
            (args.output / f"{name}-{index:02d}-stderr.txt").write_text(run.stderr, encoding="utf-8")
            run.check_returncode()
            record = json.loads(run.stdout)
            raw = target.read_bytes()
            if len(raw) != args.width * args.height * 3:
                raise ValueError("Native model output size does not match the source frame")
            outputs.append(np.frombuffer(raw, np.uint8).reshape(args.height, args.width, 3))
            records.append({"backend": record["backend"], "providerEvents": record["provider_events"],
                            "modelSeconds": record["elapsed_seconds"], "endToEndSeconds": time.monotonic() - started})
            print(f"{name}: {index + 1}/{args.frames} frames", flush=True)
        restored = np.stack(outputs)
        report["cases"][name] = {"input": metrics(sequence, reference), "restored": metrics(restored, reference), "frames": records}
        (args.output / "quality.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps(report["cases"], indent=2), flush=True)


if __name__ == "__main__":
    main()

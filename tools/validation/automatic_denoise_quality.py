"""Local, synthetic FastDVDnet conditioning evidence; never downloads weights.

This is a developer validation runner, not a distributed user CLI. Metrics on
generated clips do not qualify real-video perceptual quality or weight rights.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess

import numpy as np


def metrics(reference: np.ndarray, candidate: np.ndarray) -> dict:
    difference = candidate.astype(np.float64) - reference.astype(np.float64)
    mse = np.mean(difference * difference, axis=(1, 2, 3))
    temporal_error = np.diff(difference, axis=0)
    return {
        "mse_255": float(np.mean(difference * difference)),
        "psnr_db": None if not np.any(difference) else float(10 * math.log10(255**2 / np.mean(difference * difference))),
        "per_frame_psnr_db": [None if value == 0 else float(10 * math.log10(255**2 / value)) for value in mse],
        "max_error_255": float(np.max(np.abs(difference))),
        "temporal_reference_error_mae_255": float(np.mean(np.abs(temporal_error))) if temporal_error.size else 0,
    }


def fixtures(width: int, height: int, frames: int) -> dict[str, tuple[np.ndarray, np.ndarray, str]]:
    random = np.random.default_rng(104317)
    yy, xx = np.mgrid[:height, :width]
    smooth = np.stack([np.stack([
        70 + xx * 0.22 + yy * 0.08 + t,
        85 + xx * 0.12 + yy * 0.15 + t,
        100 + xx * 0.08 + yy * 0.12 + t,
    ], axis=-1) for t in range(frames)])
    smooth = np.rint(smooth).astype(np.uint8)
    noisy = np.clip(np.rint(smooth.astype(float) + random.normal(0, 15, smooth.shape)), 0, 255).astype(np.uint8)
    cut_reference = smooth.copy()
    cut_reference[frames // 2:] = 210 - (smooth[frames // 2:] // 3)
    cut_noisy = cut_reference.copy()
    cut_noisy[:frames // 2] = noisy[:frames // 2]
    texture = np.stack([np.stack([
        128 + 45 * np.sin(xx * 2.1 + yy * 1.3 + t * 0.2),
        128 + 40 * np.sin(xx * 1.7 - yy * 2.0 + t * 0.1),
        128 + 43 * np.cos(xx * 1.9 + yy * 2.1 + t * 0.15),
    ], axis=-1) for t in range(frames)]).round().astype(np.uint8)
    hud = smooth.copy()
    for t in range(frames):
        # Thin high-contrast lines and repeated bitmap glyph-like strokes.
        hud[t, 8:11, 8:150] = 235
        hud[t, 80:82, 25:170] = 20
        for column in range(16, 156, 12):
            hud[t, 20:42, column:column + 2] = 235
            hud[t, 20:22, column:column + 8] = 235
            hud[t, 29:31, column:column + 8] = 235
            hud[t, 40:42, column:column + 8] = 235
    grain = np.clip(np.rint(smooth.astype(float) + random.normal(0, 9, smooth.shape)), 0, 255).astype(np.uint8)
    clipped = np.broadcast_to(np.where((xx + yy) % 2 == 0, 0, 255)[None, :, :, None], smooth.shape).astype(np.uint8).copy()
    blocks = np.broadcast_to((90 + ((xx // 8 + yy // 8) % 2) * 35)[None, :, :, None], smooth.shape).astype(np.uint8).copy()
    return {
        "clean_smooth": (smooth, smooth, "Clean linear/quantized ramps; intended preservation."),
        "known_awgn_sigma15": (noisy, smooth, "IID Gaussian noise sigma 15 added to the known clean RGB reference."),
        "noisy_to_clean_scene_cut": (cut_noisy, cut_reference, "Known hard cut after the noisy first scene; clean second scene must reset conditioning."),
        "clean_high_frequency_texture": (texture, texture, "Clean synthetic fine texture: a false-positive stress case, not natural-image qualification."),
        "clean_hud_like_strokes": (hud, hud, "Clean thin lines and bitmap glyph-like strokes; not a representative game-HUD corpus."),
        "intended_film_grain": (grain, grain, "Intentional synthetic Gaussian grain is the preservation reference; denoising here is an unwanted semantic false positive."),
        "clipped_low_confidence": (clipped, clipped, "Saturated checkerboard; no trusted additive-noise supports should mean an exact bypass."),
        "synthetic_block_edges": (blocks, blocks, "Synthetic 8x8 step/block edges, not an actual codec-compression benchmark."),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validator", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--provider", choices=["cuda", "tensorrt"], default="cuda")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    width, height, count = 192, 128, 6
    environment = os.environ.copy()
    environment["PATH"] = str(args.runtime.resolve()) + os.pathsep + environment.get("PATH", "")
    report = {"schemaVersion": 1, "synthetic_only": True, "width": width, "height": height, "frames": count, "cases": {}}
    for name, (source, reference, description) in fixtures(width, height, count).items():
        case = args.output / name
        case.mkdir()
        source.tofile(case / "input.rgb")
        reference.tofile(case / "reference.rgb")
        command = [str(args.validator.resolve()), str(args.pack.resolve()), str(args.runtime.resolve()),
                   str((case / "input.rgb").resolve()), str(width), str(height), str(count), "auto",
                   str((case / "output.rgb").resolve()), args.provider]
        result = subprocess.run(command, env=environment, capture_output=True, text=True, check=False, timeout=180)
        (case / "stderr.txt").write_text(result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{name}: validator failed ({result.returncode}): {result.stderr[-2000:]}")
        native = json.loads(result.stdout)
        (case / "native.json").write_text(json.dumps(native, indent=2, allow_nan=False), encoding="utf-8")
        restored = np.fromfile(case / "output.rgb", dtype=np.uint8)
        if restored.size != source.size:
            raise RuntimeError(f"{name}: output size differs from the source-frame contract")
        restored = restored.reshape(source.shape)
        sigmas = [item["sigma255"] for item in native["frame_timings"]]
        bypass = [index for index, sigma in enumerate(sigmas) if sigma == 0]
        if any(not np.array_equal(restored[index], source[index]) for index in bypass):
            raise RuntimeError(f"{name}: sigma-zero did not preserve the actual input bytes")
        report["cases"][name] = {
            "description": description,
            "backend": native["backend"], "model_sha256": native["model_sha256"],
            "device": native["device"], "provider_events": native["provider_events"],
            "initialization_seconds": native["initialization_seconds"], "total_seconds": native["total_seconds"],
            "frame_observations": native["frame_timings"],
            "bypassed_frame_indices": bypass, "bypass_byte_exact": True,
            "input_vs_reference": metrics(reference, source), "output_vs_reference": metrics(reference, restored),
            "input_vs_output": metrics(source, restored),
        }
        print(name, json.dumps({"sigmas": sigmas, "psnr": report["cases"][name]["output_vs_reference"]["psnr_db"]}), flush=True)
    (args.output / "summary.json").write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")


if __name__ == "__main__":
    main()

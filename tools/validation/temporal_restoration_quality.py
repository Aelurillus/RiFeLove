"""Local generated-reference evaluation, native GPU + CPU five-frame ablation.

This script never reads or uploads user videos. It deliberately also tests H.264
damage and clean content so a denoiser cannot masquerade as general restoration.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import time

import numpy as np
import onnxruntime as ort


def fixture(width: int, height: int, frames: int) -> np.ndarray:
    x, y = np.meshgrid(np.arange(width), np.arange(height))
    result = []
    for index in range(frames):
        rgb = np.stack([.18 + .34 * x / width, .24 + .27 * y / height,
                        .32 + .08 * np.sin(x * .065)], axis=-1)
        # Fine static texture and two-pixel HUD lines coexist with a moving
        # colored object. The left third remains a static noise diagnostic.
        texture = (x > width * .45) & (y > height * .45)
        rgb += texture[..., None] * (.06 * np.sin(x[..., None] * .8) * np.cos(y[..., None] * .7))
        box = (x > width * .4 + index * 3) & (x < width * .6 + index * 3) & (y > height * .15) & (y < height * .38)
        rgb[box] = [.8, .35, .18]
        rgb[(y > height * .7) & (y < height * .7 + 2) & (x > width * .25)] = [.85, .85, .85]
        result.append(np.rint(np.clip(rgb, 0, 1) * 255).astype(np.uint8))
    return np.stack(result)


def metrics(output: np.ndarray, reference: np.ndarray) -> dict:
    error = output.astype(np.float64) - reference.astype(np.float64)
    mse = float(np.mean(error ** 2))
    temporal = float(np.sqrt(np.mean(np.diff(error, axis=0) ** 2))) if len(error) > 1 else None
    gx = np.mean(np.abs(np.diff(reference.astype(np.float64), axis=2)), axis=3)
    edge = gx > 8
    edge_error = np.abs(np.diff(output.astype(np.float64), axis=2) - np.diff(reference.astype(np.float64), axis=2))
    return {"psnr_db": None if mse == 0 else float(10 * np.log10(255 ** 2 / mse)),
            "rmse_levels": float(np.sqrt(mse)), "temporal_error_difference_rmse_levels": temporal,
            "static_region_noise_std_levels": float(np.std(error[:, :, :reference.shape[2] // 3])),
            "edge_gradient_mae_levels": float(np.mean(edge_error[edge])) if np.any(edge) else None}


def cpu_sequence(session, sequence, sigma, repeat_center=False):
    n, h, w, _ = sequence.shape
    hp, wp = (h + 63) // 64 * 64, (w + 63) // 64 * 64
    frames = np.pad(sequence.astype(np.float32) / 255., ((0, 0), (0, hp - h), (0, wp - w), (0, 0)), mode="edge")
    result = []
    for center in range(n):
        indices = [center] * 5 if repeat_center else [max(0, min(n - 1, center + offset)) for offset in range(-2, 3)]
        image = frames[indices].transpose(0, 3, 1, 2).reshape(1, 15, hp, wp)
        packed = np.concatenate([image, np.full((1, 1, hp, wp), sigma / 255., np.float32)], axis=1)
        restored = session.run(None, {"frames_and_noise": packed})[0][0, :, :h, :w].transpose(1, 2, 0)
        result.append(np.rint(np.clip(restored, 0, 1) * 255).astype(np.uint8))
    return np.stack(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--pack", type=Path, required=True)
    p.add_argument("--validator", type=Path, required=True)
    p.add_argument("--runtime", type=Path, required=True)
    p.add_argument("--ffmpeg", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--provider", choices=["cuda", "tensorrt"], default="cuda")
    p.add_argument("--width", type=int, default=256)
    p.add_argument("--height", type=int, default=144)
    p.add_argument("--frames", type=int, default=9)
    p.add_argument("--compression-encoder", choices=["libopenh264", "libx264"], default="libopenh264",
                   help="H.264 encoder available in the chosen FFmpeg build")
    p.add_argument("--skip-cpu", action="store_true")
    args = p.parse_args()
    out = args.output.resolve()
    if out.exists():
        raise RuntimeError("Choose a fresh report directory; existing evidence is never replaced")
    out.mkdir(parents=True)
    clean = fixture(args.width, args.height, args.frames)
    (out / "reference.rgb").write_bytes(clean.tobytes())
    rng = np.random.default_rng(419005)
    sigma = 25.
    noisy = np.rint(np.clip(clean.astype(np.float64) + rng.normal(0, sigma, clean.shape), 0, 255)).astype(np.uint8)
    (out / "noisy.rgb").write_bytes(noisy.tobytes())
    compressed = out / "compression-damage.mp4"
    compression_settings = (["-b:v", "40k", "-maxrate", "40k", "-bufsize", "80k", "-allow_skip_frames", "0"]
                            if args.compression_encoder == "libopenh264" else ["-crf", "43"])
    subprocess.run([str(args.ffmpeg), "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{args.width}x{args.height}",
                    "-r", "30", "-i", str(out / "reference.rgb"), "-c:v", args.compression_encoder, *compression_settings,
                    "-pix_fmt", "yuv420p", str(compressed)], check=True)
    subprocess.run([str(args.ffmpeg), "-v", "error", "-i", str(compressed), "-f", "rawvideo", "-pix_fmt", "rgb24", str(out / "compressed.rgb")], check=True)
    damaged = np.fromfile(out / "compressed.rgb", np.uint8).reshape(clean.shape)
    session = None
    if not args.skip_cpu:
        options = ort.SessionOptions(); options.intra_op_num_threads = 4
        session = ort.InferenceSession(str(args.pack / "model.onnx"), sess_options=options, providers=["CPUExecutionProvider"])
    cases = []
    for name, sequence in [("noisy", noisy), ("compressed", damaged), ("clean", clean)]:
        input_path = out / ("reference.rgb" if name == "clean" else f"{name}.rgb")
        output_path = out / f"{name}-{args.provider}.rgb"
        completed = subprocess.run([str(args.validator), str(args.pack), str(args.runtime), str(input_path), str(args.width), str(args.height),
                                    str(args.frames), str(sigma), str(output_path), args.provider], capture_output=True, text=True)
        (out / f"{name}-stderr.txt").write_text(completed.stderr, encoding="utf-8")
        completed.check_returncode()
        native = json.loads(completed.stdout)
        restored = np.fromfile(output_path, np.uint8).reshape(clean.shape)
        case = {"case": name, "input": metrics(sequence, clean), "restored": metrics(restored, clean), "native": native}
        if session:
            started = time.perf_counter()
            cpu = cpu_sequence(session, sequence, sigma)
            repeated = cpu_sequence(session, sequence, sigma, repeat_center=True)
            (out / f"{name}-cpu.rgb").write_bytes(cpu.tobytes())
            case["cpu_five_frame"] = metrics(cpu, clean)
            case["cpu_repeated_center_ablation"] = metrics(repeated, clean)
            case["cpu_seconds_including_ablation"] = time.perf_counter() - started
            case["native_vs_cpu_maximum_error_levels"] = int(np.max(np.abs(restored.astype(np.int16) - cpu.astype(np.int16))))
            case["temporal_context_dependency_rms_levels"] = float(np.sqrt(np.mean((cpu.astype(np.float64) - repeated) ** 2)))
        cases.append(case)
        report = {"schemaVersion": 1, "fixture": "Generated RGB gradient, fine texture, moving object and HUD; clipped AWGN sigma25 and lossy H.264",
                  "compression": {"encoder": args.compression_encoder, "settings": compression_settings},
                  "limitations": "Small synthetic reference only. Temporal error difference subtracts known clean motion; not optical-flow perceptual flicker or LPIPS. Fixed sigma25 deliberately tests misuse on compressed/clean video. No user-video quality, HDR, deblock/detail or product qualification.",
                  "cases": cases}
        (out / "quality-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"case": name, "input": case["input"], "restored": case["restored"], "native_fps": native["frames_per_second"]}), flush=True)


if __name__ == "__main__":
    main()

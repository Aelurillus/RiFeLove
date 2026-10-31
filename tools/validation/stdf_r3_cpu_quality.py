"""Local synthetic compression controls for the experimental STDF-R3 graph.

CPU only; generated raw-Y fixtures, software encoders, no user clips or network.
Results describe these fixtures only, not codec-wide or perceptual qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np
import onnxruntime as ort


def scene_window(center: int, frames: int, cuts: list[int], repeat=False) -> list[int]:
    """First sample of each new scene is in cuts; never borrow across a cut."""
    if not 0 <= center < frames or any(c <= 0 or c >= frames for c in cuts):
        raise ValueError("Invalid scene boundary or frame index")
    start = max([0] + [cut for cut in cuts if cut <= center])
    end = min([frames] + [cut for cut in cuts if cut > center]) - 1
    return [center] * 7 if repeat else [max(start, min(end, center + delta)) for delta in range(-3, 4)]


def fixture(width: int, height: int, frames: int, cut: int) -> np.ndarray:
    x, y = np.meshgrid(np.arange(width), np.arange(height))
    sequence = []
    for index in range(frames):
        image = 30. + 95. * x / width + 36. * y / height
        image += ((x > width * .43) & (y > height * .5)) * (26 * np.sin(x * .8) * np.cos(y * .75))
        moving = (x > width * .2 + index * 3) & (x < width * .46 + index * 3) & (y > height * .2) & (y < height * .42)
        image[moving] = 205.
        image[(y % 17 < 2) & (x > width * .55)] = 215.
        if index >= cut:
            image = 245. - image
        sequence.append(np.rint(np.clip(image, 16, 235)).astype(np.uint8))
    return np.stack(sequence)


def metrics(output: np.ndarray, reference: np.ndarray) -> dict:
    error = output.astype(np.float64) - reference
    mse = float(np.mean(error ** 2))
    dx_ref, dy_ref = np.diff(reference.astype(float), axis=2), np.diff(reference.astype(float), axis=1)
    dx, dy = np.diff(output.astype(float), axis=2), np.diff(output.astype(float), axis=1)
    edge_errors = np.concatenate((np.abs(dx - dx_ref)[np.abs(dx_ref) > 8], np.abs(dy - dy_ref)[np.abs(dy_ref) > 8]))
    return {"psnr_y_db": None if mse == 0 else float(10 * np.log10(255 ** 2 / mse)),
            "ssim_y_valid_11x11_gaussian": ssim(output, reference),
            "rmse_y_levels": float(np.sqrt(mse)),
            "edge_gradient_mae_levels": float(np.mean(edge_errors)) if edge_errors.size else None,
            "error_temporal_difference_rmse_levels": float(np.sqrt(np.mean(np.diff(error, axis=0) ** 2))),
            "maximum_absolute_error_levels": float(np.max(np.abs(error)))}


def ssim(output: np.ndarray, reference: np.ndarray) -> float:
    """11x11 sigma=1.5 Gaussian SSIM, valid centers, population covariance."""
    a, b = output.astype(np.float64), reference.astype(np.float64)
    coordinates = np.arange(11) - 5
    kernel = np.exp(-(coordinates ** 2) / (2 * 1.5 ** 2)); kernel /= kernel.sum()

    def smooth(values):
        horizontal = np.lib.stride_tricks.sliding_window_view(values, 11, axis=2) @ kernel
        return np.lib.stride_tricks.sliding_window_view(horizontal, 11, axis=1) @ kernel

    ma, mb = smooth(a), smooth(b)
    va, vb, covariance = smooth(a * a) - ma * ma, smooth(b * b) - mb * mb, smooth(a * b) - ma * mb
    c1, c2 = (.01 * 255) ** 2, (.03 * 255) ** 2
    return float(np.mean(((2 * ma * mb + c1) * (2 * covariance + c2)) /
                         ((ma * ma + mb * mb + c1) * (va + vb + c2))))


def restore(session, sequence: np.ndarray, cuts: list[int], repeat=False):
    n, h, w = sequence.shape
    frames = sequence.astype(np.float32) / 255.
    padded = np.pad(frames, ((0, 0), (0, (-h) % 8), (0, (-w) % 8)), mode="edge")
    result = []
    for center in range(n):
        window = scene_window(center, n, cuts, repeat=repeat)
        output = session.run(None, {"luma_frames": padded[window][None]})[0]
        if output.shape != (1, 1, padded.shape[1], padded.shape[2]) or not np.isfinite(output).all():
            raise RuntimeError("Unexpected or non-finite model output")
        result.append(np.clip(output[0, 0, :h, :w] * 255., 0, 255))
    return np.stack(result)


def file_sha(path: Path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=192)
    parser.add_argument("--height", type=int, default=128)
    parser.add_argument("--frames", type=int, default=9)
    args = parser.parse_args()
    if args.width < 32 or args.height < 32 or args.width % 2 or args.height % 2 or args.frames < 7:
        raise ValueError("Use even YUV420 dimensions >=32 and at least seven frames")
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory")
    args.output.mkdir(parents=True)
    cut = args.frames // 2
    clean = fixture(args.width, args.height, args.frames, cut)
    reference = args.output / "generated-reference.yuv"
    with reference.open("wb") as handle:
        for frame in clean:
            handle.write(frame.tobytes())
            handle.write(bytes([128]) * (args.width * args.height // 2))
    options = ort.SessionOptions(); options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(args.graph), sess_options=options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise RuntimeError("This validation must not use the GPU")
    cases = []
    configs = [("clean", None, []),
               ("h264-16kbps", "libopenh264", ["-b:v", "16k", "-allow_skip_frames", "0"]),
               ("h264-80kbps", "libopenh264", ["-b:v", "80k", "-allow_skip_frames", "0"]),
               ("hevc-kvazaar-qp37", "libkvazaar", ["-kvazaar-params", "qp=37:threads=2:owf=0"]),
               ("hevc-kvazaar-qp27", "libkvazaar", ["-kvazaar-params", "qp=27:threads=2:owf=0"]),
               ("av1-aom-crf40", "libaom-av1", ["-crf", "40", "-b:v", "0", "-cpu-used", "8", "-threads", "2"])]
    report = {"schemaVersion": 1, "status": "running_cpu_fixture_evaluation", "model_sha256": file_sha(args.graph),
              "ffmpeg_sha256": file_sha(args.ffmpeg),
              "ffmpeg_version": subprocess.check_output([str(args.ffmpeg), "-version"], text=True).splitlines()[0],
              "fixture": {"width": args.width, "height": args.height, "frames": args.frames,
                          "known_scene_cuts": [cut], "fps": "30/1", "color": "bt709 limited-range neutral-chroma YUV420P",
                          "description": "Generated gradients, fine texture, sharp lines, moving object, hard cut",
                          "reference_sha256": file_sha(reference)},
              "provider": session.get_providers(), "cases": cases,
              "limitations": ["Small synthetic grayscale SDR footage, not authorized real-content testing",
                              "HEVC uses Kvazaar, not the model's HM16.5 LDP training encoder",
                              "Y-only metrics; chroma is neutral and unchanged; no HDR/color qualification",
                              "Temporal error metric subtracts known clean motion; not optical-flow or perceptual flicker",
                              "Blends are explicit fixed strengths, not a validated automatic estimator",
                              "No native CUDA/TensorRT integration, weight redistribution or product-quality claim"]}
    for name, encoder, settings in configs:
        commands = []
        if encoder:
            clip = args.output / f"{name}.mkv"
            encode = [str(args.ffmpeg), "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pixel_format", "yuv420p",
                      "-video_size", f"{args.width}x{args.height}", "-framerate", "30", "-i", str(reference),
                      "-an", "-c:v", encoder, *settings, "-color_primaries", "bt709", "-color_trc", "bt709",
                      "-colorspace", "bt709", "-color_range", "tv", str(clip)]
            decode = [str(args.ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(clip),
                      "-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "yuv420p", "pipe:1"]
            subprocess.run(encode, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            raw = subprocess.check_output(decode)
            frame_bytes = args.width * args.height * 3 // 2
            if len(raw) != args.frames * frame_bytes:
                raise RuntimeError("Codec output changed frame count or geometry")
            damaged = np.frombuffer(raw, np.uint8).reshape(args.frames, frame_bytes)[:, :args.width * args.height].reshape(clean.shape)
            commands = [encode, decode]
        else:
            damaged = clean
        start = time.perf_counter()
        restored = restore(session, damaged, [cut])
        elapsed = time.perf_counter() - start
        repeated = restore(session, damaged, [cut], repeat=True)
        blends = {}
        for strength in [.25, .5, 1.]:
            blended = np.rint(damaged.astype(float) + strength * (restored - damaged)).astype(np.uint8)
            blends[str(strength)] = metrics(blended, clean)
        case = {"name": name, "encoder": encoder, "settings": settings,
                "commands": commands, "input": metrics(damaged, clean), "restored": blends["1.0"],
                "blends": blends, "cpu_inference_seconds": elapsed,
                "cpu_inference_fps": args.frames / elapsed,
                "repeated_center_ablation": metrics(np.rint(repeated).astype(np.uint8), clean),
                "temporal_context_dependency_rms_levels": float(np.sqrt(np.mean((restored - repeated) ** 2))),
                "input_sha256": hashlib.sha256(damaged.tobytes()).hexdigest(),
                "restored_luma_sha256": hashlib.sha256(np.rint(restored).astype(np.uint8).tobytes()).hexdigest()}
        cases.append(case)
        (args.output / "quality.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"name": name, "input": case["input"], "restored": case["restored"],
                          "context_delta": case["temporal_context_dependency_rms_levels"], "cpu_fps": case["cpu_inference_fps"]}), flush=True)
    report["status"] = "completed_cpu_fixture_evaluation_not_product_acceptance"
    (args.output / "quality.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

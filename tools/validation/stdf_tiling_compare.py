"""Measure STDF whole-frame versus overlapped tile inference on CPU.

This is an offline qualification tool.  It never changes the product pipeline,
downloads a checkpoint, or writes into an existing evidence directory.  The
whole-frame ONNX result is the oracle for each generated input.  A tile is
expanded by a caller-selected halo, clamped to the real image at outer
boundaries, and cropped back to its core.  Because STDF predicts deformable
offsets from the tile itself, a halo value is an empirical qualification
parameter, not a proof of exactness.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _checkerboard(height: int, width: int, *, period: int = 8) -> np.ndarray:
    yy, xx = np.indices((height, width))
    return (((yy // period + xx // period) & 1).astype(np.float32))


def _texture(height: int, width: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    noise = rng.random((height, width), dtype=np.float32)
    yy, xx = np.indices((height, width), dtype=np.float32)
    smooth = (0.45 + 0.22 * np.sin(xx / 5.7) + 0.17 * np.cos(yy / 7.9)
              + 0.11 * np.sin((xx + yy) / 13.0))
    return np.clip(0.55 * noise + 0.45 * smooth, 0.0, 1.0).astype(np.float32)


def _moving_square(height: int, width: int, shift: int, *, hard_edge: bool) -> np.ndarray:
    base = np.zeros((height, width), dtype=np.float32)
    yy, xx = np.indices((height, width))
    base[...] = 0.08 + 0.12 * (xx / max(width - 1, 1))
    size = max(12, min(height, width) // 4)
    left = max(2, width // 4 + shift)
    top = max(2, height // 3)
    inside = (xx >= left) & (xx < left + size) & (yy >= top) & (yy < top + size)
    base[inside] = 0.9
    if not hard_edge:
        # A mild texture inside the moving object makes seams visible beyond a
        # single high-contrast rectangle.
        base[inside] += 0.06 * np.sin(xx[inside] / 2.3) * np.cos(yy[inside] / 3.1)
    return np.clip(base, 0.0, 1.0).astype(np.float32)


def make_cases(height: int, width: int) -> list[tuple[str, np.ndarray]]:
    rng = np.random.default_rng(876051)
    cases: list[tuple[str, np.ndarray]] = []

    # The center input is index 3.  Each case is a seven-frame temporal window.
    noise = rng.random((7, height, width), dtype=np.float32)
    cases.append(("independent_noise", noise))
    static = _texture(height, width, 991)
    cases.append(("static_texture", np.repeat(static[None, ...], 7, axis=0)))
    moving = np.stack([_moving_square(height, width, shift=(i - 3) * max(3, width // 20),
                                       hard_edge=False) for i in range(7)])
    cases.append(("moving_textured_square", moving))
    hard = np.stack([_moving_square(height, width, shift=(i - 3) * max(5, width // 10),
                                     hard_edge=True) for i in range(7)])
    cases.append(("large_displacement_edge", hard))
    checker = _checkerboard(height, width, period=max(4, min(height, width) // 20))
    cases.append(("high_frequency_checkerboard", np.stack([
        np.roll(checker, (i - 3) * 2, axis=(0, 1)) for i in range(7)
    ])))
    cut_a = _texture(height, width, 1201)
    cut_b = _checkerboard(height, width, period=max(3, min(height, width) // 24))
    cut = np.stack([cut_a if i < 3 else cut_b for i in range(7)])
    cases.append(("hard_scene_cut", cut))
    return [(name, np.ascontiguousarray(frames[None, ...], dtype=np.float32))
            for name, frames in cases]


def _pad_tile(frames: np.ndarray, x0: int, y0: int, x1: int, y1: int,
              halo: int) -> tuple[np.ndarray, tuple[int, int, int, int]]:
    """Return a clamped source tile and its core crop.

    Do not materialize zero-valued pixels beyond the real image.  A normal
    convolution on those pixels would still produce bias/ReLU activations and
    therefore would not match the source frame's own boundary padding.
    """
    _, channels, height, width = frames.shape
    sx0, sy0 = max(0, x0 - halo), max(0, y0 - halo)
    sx1, sy1 = min(width, x1 + halo), min(height, y1 + halo)
    tile = np.ascontiguousarray(frames[:, :, sy0:sy1, sx0:sx1], dtype=np.float32)
    return tile, (x0 - sx0, y0 - sy0, x1 - sx0, y1 - sy0)


def tiled_infer(session: ort.InferenceSession, frames: np.ndarray,
                tile_w: int, tile_h: int, halo: int) -> tuple[np.ndarray, int]:
    _, _, height, width = frames.shape
    if min(tile_w, tile_h) <= 0 or min(tile_w, tile_h) % 8 or halo % 8:
        raise ValueError("tile width/height and halo must be positive multiples of 8")
    output = np.empty((1, 1, height, width), dtype=np.float32)
    count = 0
    for y0 in range(0, height, tile_h):
        for x0 in range(0, width, tile_w):
            x1, y1 = min(x0 + tile_w, width), min(y0 + tile_h, height)
            # A partial final tile is legal only if its clamped source extent
            # remains a multiple of eight.  The generated test sizes are
            # chosen so that this is true; fail closed for an unsafe shape.
            tile, crop = _pad_tile(frames, x0, y0, x1, y1, halo)
            if tile.shape[2] % 8 or tile.shape[3] % 8:
                raise ValueError(f"expanded tile shape {tile.shape[2:]} is not divisible by 8")
            inferred = session.run(None, {"luma_frames": tile})[0]
            cx0, cy0, cx1, cy1 = crop
            output[:, :, y0:y1, x0:x1] = inferred[:, :, cy0:cy1, cx0:cx1]
            count += 1
    return output, count


def _psnr(reference: np.ndarray, actual: np.ndarray) -> float | None:
    mse = float(np.mean((reference.astype(np.float64) - actual.astype(np.float64)) ** 2))
    return None if mse == 0 else 10.0 * math.log10(1.0 / mse)


def metrics(reference: np.ndarray, actual: np.ndarray, tile_w: int, tile_h: int) -> dict:
    delta = np.abs(reference.astype(np.float64) - actual.astype(np.float64))[0, 0]
    height, width = delta.shape
    seam = np.zeros_like(delta, dtype=bool)
    for x in range(tile_w, width, tile_w):
        seam[:, max(0, x - 2):min(width, x + 2)] = True
    for y in range(tile_h, height, tile_h):
        seam[max(0, y - 2):min(height, y + 2), :] = True
    nonseam = ~seam
    quant_reference = np.rint(np.clip(reference[0, 0], 0.0, 1.0) * 255.0).astype(np.int16)
    quant_actual = np.rint(np.clip(actual[0, 0], 0.0, 1.0) * 255.0).astype(np.int16)
    quant_delta = np.abs(quant_reference - quant_actual)
    return {
        "maximum_absolute_error": float(np.max(delta)),
        "mean_absolute_error": float(np.mean(delta)),
        "p99_absolute_error": float(np.quantile(delta, 0.99)),
        "psnr_db": _psnr(reference, actual),
        "changed_8bit_pixels": int(np.count_nonzero(quant_delta)),
        "maximum_8bit_difference": int(np.max(quant_delta)),
        "seam_mean_absolute_error": float(np.mean(delta[seam])) if np.any(seam) else 0.0,
        "nonseam_mean_absolute_error": float(np.mean(delta[nonseam])) if np.any(nonseam) else 0.0,
        "seam_p99_absolute_error": float(np.quantile(delta[seam], 0.99)) if np.any(seam) else 0.0,
        "seam_pixels": int(np.count_nonzero(seam)),
    }


def run(args: argparse.Namespace) -> dict:
    graph = args.graph.resolve()
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory; refusing to overwrite existing evidence")
    args.output.mkdir(parents=True)
    session_options = ort.SessionOptions()
    session_options.intra_op_num_threads = args.threads
    session = ort.InferenceSession(str(graph), sess_options=session_options,
                                   providers=["CPUExecutionProvider"])
    # The U-Net concat geometry requires dimensions divisible by 8.  Keep this
    # explicit rather than silently resizing the caller's frames.
    if args.height % 8 or args.width % 8:
        raise ValueError("height and width must be divisible by 8")
    cases = make_cases(args.height, args.width)
    tiles = [(tw, th, halo) for tw, th in args.tile for halo in args.halo]
    report: dict = {
        "schemaVersion": 1,
        "status": "running_cpu_tiling_qualification",
        "graph": str(graph),
        "graph_sha256": sha256(graph),
        "shape": [1, 7, args.height, args.width],
        "oracle": "whole-frame ONNX Runtime CPUExecutionProvider output",
        "model_contract": {"input": "FP32 [1,7,H,W]", "output": "FP32 [1,1,H,W]",
                           "dimensions_multiple_of": 8,
                           "tile_boundary_policy": "clamped source extent; model-owned padding"},
        "tile_policy": {"tiles": [list(item) for item in tiles],
                        "halo_is_empirical": True,
                        "deformable_offsets_recomputed_per_tile": True},
        "cases": [],
        "limitations": [
            "Generated Y-like tensors only; no encoded-video or color qualification",
            "Whole-frame output is the consistency oracle, not a ground-truth quality label",
            "A passing halo on these cases does not prove safety for arbitrary motion or offsets",
            "No native CUDA memory measurement is performed by this CPU-only tool",
        ],
    }
    for case_name, frames in cases:
        started = time.perf_counter()
        reference = session.run(None, {"luma_frames": frames})[0]
        full_seconds = time.perf_counter() - started
        case_report = {"name": case_name, "whole_frame_seconds": full_seconds, "results": []}
        for tile_w, tile_h, halo in tiles:
            started = time.perf_counter()
            actual, count = tiled_infer(session, frames, tile_w, tile_h, halo)
            elapsed = time.perf_counter() - started
            result = {"tile_width": tile_w, "tile_height": tile_h, "halo": halo,
                      "tile_count": count, "tiled_seconds": elapsed,
                      "speed_ratio_vs_whole_frame": elapsed / full_seconds,
                      **metrics(reference, actual, tile_w, tile_h)}
            # A conservative, explicit gate used only to label evidence.  The
            # report still retains all measurements for cases that fail it.
            result["passed_strict_consistency"] = (
                result["maximum_absolute_error"] <= args.max_error and
                result["maximum_8bit_difference"] <= args.max_8bit)
            case_report["results"].append(result)
        report["cases"].append(case_report)
        (args.output / "tiling-progress.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"case": case_name, "whole_frame_seconds": full_seconds,
                          "results": len(case_report["results"])},), flush=True)
    report["status"] = "completed_cpu_tiling_qualification"
    (args.output / "tiling-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--height", type=int, default=128)
    parser.add_argument("--width", type=int, default=192)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--tile", type=int, nargs=2, action="append", metavar=("WIDTH", "HEIGHT"),
                        help="tile width and tile height, both multiples of 8")
    parser.add_argument("--halo", type=int, action="append", default=None)
    parser.add_argument("--max-error", type=float, default=3e-5)
    parser.add_argument("--max-8bit", type=int, default=1)
    args = parser.parse_args()
    if args.tile is None:
        args.tile = [(64, 64), (96, 64), (128, 128)]
    args.tile = [(item[0], item[1]) for item in args.tile]
    if args.halo is None:
        args.halo = [0, 8, 16, 24, 32, 48, 64]
    report = run(args)
    print(json.dumps({"status": report["status"], "report": str(args.output / "tiling-report.json")}))


if __name__ == "__main__":
    main()

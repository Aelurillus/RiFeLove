"""Memory-bounded STDF tile qualification for larger synthetic frames.

The runner keeps only one clamped seven-frame tile, one output tile and one
comparison core in memory. A small reference shape may additionally run the
complete ONNX graph to calibrate the tile policy. Larger 1080p/4K shapes are sampled without
allocating a full-frame tensor or output.  Results are consistency evidence,
not a ground-truth quality score or a native CUDA memory qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

import numpy as np
import onnxruntime as ort

sys.path.insert(0, str(Path(__file__).resolve().parent))


CASE_NAMES = ("moving_textured_square", "large_displacement_edge", "high_frequency_checkerboard")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _hash_noise(y: np.ndarray, x: np.ndarray, frame: int) -> np.ndarray:
    value = (np.asarray(x, dtype=np.uint64) * np.uint64(0x9E3779B1)
             ^ np.asarray(y, dtype=np.uint64) * np.uint64(0x85EBCA77)
             ^ np.uint64((frame + 1) * 0xC2B2AE3D))
    value ^= value >> np.uint64(16)
    value *= np.uint64(0x7FEB352D)
    value ^= value >> np.uint64(15)
    return ((value & np.uint64(0xFFFFFFFF)).astype(np.float64) / 4294967295.0).astype(np.float32)


def _texture(y: np.ndarray, x: np.ndarray) -> np.ndarray:
    return np.clip(0.45 + 0.22 * np.sin(x / 5.7) + 0.17 * np.cos(y / 7.9)
                   + 0.11 * np.sin((x + y) / 13.0), 0.0, 1.0).astype(np.float32)


def case_tile(name: str, height: int, width: int, x0: int, y0: int,
              x1: int, y1: int) -> np.ndarray:
    """Generate one deterministic seven-frame tile from global coordinates."""
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        raise ValueError("invalid tile extent")
    yy_i, xx_i = np.indices((y1 - y0, x1 - x0), dtype=np.int64)
    yy = (yy_i + y0).astype(np.float32)
    xx = (xx_i + x0).astype(np.float32)
    frames = []
    for index in range(7):
        if name == "moving_textured_square":
            image = 0.08 + 0.12 * (xx / max(width - 1, 1))
            shift = (index - 3) * max(3, width // 20)
            left = max(2, width // 4 + shift)
            top = max(2, height // 3)
            size = max(12, min(height, width) // 4)
            inside = (xx >= left) & (xx < left + size) & (yy >= top) & (yy < top + size)
            image = image.astype(np.float32)
            image[inside] = 0.9 + 0.06 * np.sin(xx[inside] / 2.3) * np.cos(yy[inside] / 3.1)
        elif name == "large_displacement_edge":
            image = np.full(xx.shape, 0.08, dtype=np.float32)
            shift = (index - 3) * max(5, width // 10)
            left = max(2, width // 4 + shift)
            top = max(2, height // 3)
            size = max(12, min(height, width) // 4)
            inside = (xx >= left) & (xx < left + size) & (yy >= top) & (yy < top + size)
            image[inside] = 0.9
        elif name == "high_frequency_checkerboard":
            period = max(4, min(height, width) // 20)
            image = (((((yy.astype(np.int64) + (index - 3) * 2) // period)
                       + ((xx.astype(np.int64) + (index - 3) * 2) // period)) & 1)
                     .astype(np.float32))
        else:
            raise ValueError(f"unknown generated case: {name}")
        frames.append(np.clip(image, 0.0, 1.0).astype(np.float32))
    return np.ascontiguousarray(np.stack(frames, axis=0)[None, ...], dtype=np.float32)


def iter_tiles(height: int, width: int, tile_w: int, tile_h: int):
    if min(height, width, tile_w, tile_h) <= 0 or any(v % 8 for v in (height, width, tile_w, tile_h)):
        raise ValueError("frame and tile dimensions must be positive multiples of 8")
    for y0 in range(0, height, tile_h):
        for x0 in range(0, width, tile_w):
            yield (x0, y0, min(x0 + tile_w, width), min(y0 + tile_h, height))


def select_tiles(tiles: list[tuple[int, int, int, int]], limit: int | None):
    if limit is None or limit >= len(tiles):
        return tiles
    if limit <= 0:
        raise ValueError("sample tile limit must be positive")
    wanted = [0, len(tiles) - 1, len(tiles) // 2]
    wanted.extend(np.linspace(0, len(tiles) - 1, num=limit, dtype=int).tolist())
    selected = []
    for index in wanted:
        if index not in selected:
            selected.append(index)
        if len(selected) == limit:
            break
    return [tiles[index] for index in selected]


class Accumulator:
    def __init__(self):
        self.sum = 0.0
        self.count = 0
        self.maximum = 0.0
        self.maximum_8bit = 0
        self.changed_8bit = 0
        self.seam_sum = 0.0
        self.seam_count = 0
        self.seam_maximum = 0.0

    def add(self, reference: np.ndarray, actual: np.ndarray,
            seam_mask: np.ndarray | None = None):
        delta = np.abs(reference.astype(np.float64) - actual.astype(np.float64))[0, 0]
        self.sum += float(np.sum(delta))
        self.count += int(delta.size)
        self.maximum = max(self.maximum, float(np.max(delta)))
        ref8 = np.rint(np.clip(reference[0, 0], 0, 1) * 255).astype(np.int16)
        act8 = np.rint(np.clip(actual[0, 0], 0, 1) * 255).astype(np.int16)
        delta8 = np.abs(ref8 - act8)
        self.maximum_8bit = max(self.maximum_8bit, int(np.max(delta8)))
        self.changed_8bit += int(np.count_nonzero(delta8))
        if seam_mask is not None and np.any(seam_mask):
            values = delta[seam_mask]
            self.seam_sum += float(np.sum(values))
            self.seam_count += int(values.size)
            self.seam_maximum = max(self.seam_maximum, float(np.max(values)))

    def result(self) -> dict:
        return {
            "mean_absolute_error": self.sum / self.count if self.count else 0.0,
            "maximum_absolute_error": self.maximum,
            "maximum_8bit_difference": self.maximum_8bit,
            "changed_8bit_pixels": self.changed_8bit,
            "seam_mean_absolute_error": self.seam_sum / self.seam_count if self.seam_count else 0.0,
            "seam_maximum_absolute_error": self.seam_maximum,
            "seam_pixels": self.seam_count,
        }


def seam_mask(tile: tuple[int, int, int, int], height: int, width: int, radius: int = 2):
    x0, y0, x1, y1 = tile
    mask = np.zeros((y1 - y0, x1 - x0), dtype=bool)
    if x0 > 0:
        mask[:, :radius] = True
    if x1 < width:
        mask[:, -radius:] = True
    if y0 > 0:
        mask[:radius, :] = True
    if y1 < height:
        mask[-radius:, :] = True
    return mask


def run_shape(session: ort.InferenceSession, height: int, width: int,
              tile_w: int, tile_h: int, halos: list[int], cases: list[str],
              reference: bool, sample_limit: int | None) -> dict:
    if not halos or len(set(halos)) != len(halos) or any(halo < 0 or halo % 8 for halo in halos):
        raise ValueError("halos must be distinct non-negative multiples of 8")
    all_tiles = list(iter_tiles(height, width, tile_w, tile_h))
    selected = select_tiles(all_tiles, sample_limit)
    case_reports = []
    for case_name in cases:
        oracle = None
        full_seconds = None
        if reference:
            full_input = case_tile(case_name, height, width, 0, 0, width, height)
            started = time.perf_counter()
            oracle = session.run(None, {"luma_frames": full_input})[0]
            full_seconds = time.perf_counter() - started
        aggregates = {halo: Accumulator() for halo in halos}
        elapsed = {halo: 0.0 for halo in halos}
        peak_tensor_bytes = {halo: 0 for halo in halos}
        peak_tracked_bytes = {halo: 0 for halo in halos}
        pair_aggregate = Accumulator() if len(halos) >= 2 else None
        # Compare both policies while the current tile is alive. Retaining
        # cropped NumPy views for every tile would also retain every full ORT
        # output allocation, so a supposedly bounded 4K pass would grow with
        # the entire image. The copied first core is released after its pair.
        for tile in selected:
            first_core = None
            for halo_index, halo in enumerate(halos):
                started = time.perf_counter()
                x0, y0, x1, y1 = tile
                sx0, sy0 = max(0, x0 - halo), max(0, y0 - halo)
                sx1, sy1 = min(width, x1 + halo), min(height, y1 + halo)
                source = case_tile(case_name, height, width, sx0, sy0, sx1, sy1)
                output = session.run(None, {"luma_frames": source})[0]
                cx0, cy0, cx1, cy1 = x0 - sx0, y0 - sy0, x1 - sx0, y1 - sy0
                core = output[:, :, cy0:cy1, cx0:cx1]
                if oracle is not None:
                    ref_core = oracle[:, :, y0:y1, x0:x1]
                    aggregates[halo].add(ref_core, core, seam_mask(tile, height, width))
                if pair_aggregate is not None and halo_index == 0:
                    first_core = core.copy()
                elif pair_aggregate is not None and halo_index == 1:
                    pair_aggregate.add(first_core, core, seam_mask(tile, height, width))
                tensor_bytes = source.nbytes + output.nbytes
                peak_tensor_bytes[halo] = max(peak_tensor_bytes[halo], tensor_bytes)
                peak_tracked_bytes[halo] = max(peak_tracked_bytes[halo],
                                               tensor_bytes + (first_core.nbytes if first_core is not None else 0))
                elapsed[halo] += time.perf_counter() - started
                if halo_index == 1:
                    first_core = None
                del core, output, source
        policy_reports = []
        for halo in halos:
            report = {
                "halo": halo,
                "tiles_evaluated": len(selected),
                "tiles_total": len(all_tiles),
                "sampled": len(selected) < len(all_tiles),
                "elapsed_seconds": elapsed[halo],
                "seconds_per_tile": elapsed[halo] / len(selected),
                "peak_host_tensor_bytes": peak_tensor_bytes[halo],
                "peak_host_tensor_mib": peak_tensor_bytes[halo] / (1024 * 1024),
                "peak_tracked_host_tensor_bytes": peak_tracked_bytes[halo],
            }
            if oracle is not None:
                report["vs_whole_frame"] = aggregates[halo].result()
            policy_reports.append(report)
        pair_reports = []
        if pair_aggregate is not None:
            first, second = halos[0], halos[1]
            pair_reports.append({"halo_a": first, "halo_b": second,
                                 "interpretation": "pair consistency, not whole-frame correctness",
                                 **pair_aggregate.result()})
        case_reports.append({"name": case_name, "whole_frame_seconds": full_seconds,
                             "policies": policy_reports, "pair_consistency": pair_reports})
    return {"height": height, "width": width, "tile": [tile_w, tile_h],
            "tiles_total": len(all_tiles), "tiles_evaluated": len(selected),
            "sampled": len(selected) < len(all_tiles), "cases": case_reports}


def run(args: argparse.Namespace) -> dict:
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory; refusing to overwrite existing evidence")
    args.output.mkdir(parents=True)
    session_options = ort.SessionOptions()
    session_options.intra_op_num_threads = args.threads
    session = ort.InferenceSession(str(args.graph.resolve()), sess_options=session_options,
                                   providers=["CPUExecutionProvider"])
    reference_shape = tuple(args.reference_shape) if args.reference_shape else None
    shapes = [tuple(item) for item in args.shape]
    report = {
        "schemaVersion": 1,
        "status": "running_memory_bounded_cpu_qualification",
        "graph_sha256": sha256(args.graph.resolve()),
        "provider": "CPUExecutionProvider",
        "tile": [args.tile[0], args.tile[1]],
        "halos": args.halo,
        "cases": args.case,
        "reference_shape": list(reference_shape) if reference_shape else None,
        "memory_accounting": "peak_host_tensor_bytes counts the source and full output tile; peak_tracked_host_tensor_bytes also includes the copied comparison core. Generator and metric temporaries, any whole-frame oracle and provider arena are excluded",
        "shapes": [],
        "limitations": [
            "The generated cases are deterministic Y-like tensors, not real encoded video",
            "Large shapes are sampled and have no whole-frame oracle",
            "Halo pair consistency is evidence of truncation stability, not proof of full-frame correctness",
            "No native CUDA peak-memory or throughput claim is made",
        ],
    }
    for height, width in shapes:
        if reference_shape and (height, width) == reference_shape:
            use_reference = True
        else:
            use_reference = False
        result = run_shape(session, height, width, args.tile[0], args.tile[1],
                           args.halo, args.case, use_reference, args.sample_tiles)
        report["shapes"].append(result)
        (args.output / "bounded-progress.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"shape": [height, width], "tiles": result["tiles_evaluated"],
                          "reference": use_reference}), flush=True)
    report["status"] = "completed_memory_bounded_cpu_qualification"
    (args.output / "bounded-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--shape", type=int, nargs=2, action="append", required=True,
                        metavar=("HEIGHT", "WIDTH"))
    parser.add_argument("--reference-shape", type=int, nargs=2, metavar=("HEIGHT", "WIDTH"))
    parser.add_argument("--tile", type=int, nargs=2, default=[384, 256], metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--halo", type=int, nargs="+", default=[48, 64])
    parser.add_argument("--case", action="append", choices=CASE_NAMES, default=None)
    parser.add_argument("--sample-tiles", type=int, default=6)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    if args.case is None:
        args.case = list(CASE_NAMES)
    run(args)


if __name__ == "__main__":
    main()

"""Measure STDF's learned deformable-offset range on generated windows.

The probe is evidence for selecting a conservative tile halo.  It is not a
proof of a finite receptive field: the offset head itself is a U-Net and is
recomputed for each tile, so an observed offset percentile cannot guarantee
whole-frame equivalence for unseen content.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "model_export"))
from stdf_r3_portable import load_source_model, sha
from stdf_tiling_compare import make_cases


def run(args: argparse.Namespace) -> dict:
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory; refusing to overwrite existing evidence")
    args.output.mkdir(parents=True)
    model = load_source_model(args.source, args.checkpoint)
    captured: list[torch.Tensor] = []

    def hook(_module, _inputs, output):
        captured.append(output.detach().cpu())

    handle = model.ffnet.offset_mask.register_forward_hook(hook)
    cases = []
    try:
        with torch.inference_mode():
            for name, frames in make_cases(args.height, args.width):
                captured.clear()
                model(torch.from_numpy(frames))
                if len(captured) != 1:
                    raise RuntimeError("offset head hook did not capture exactly one output")
                raw = captured[0].numpy()
                offsets = raw[:, :126, :, :].reshape(1, 7, 2, 9, args.height, args.width)
                absolute = np.abs(offsets.astype(np.float64))
                radial = np.sqrt((offsets[:, :, 0] ** 2) + (offsets[:, :, 1] ** 2))
                cases.append({
                    "name": name,
                    "maximum_absolute_component_px": float(np.max(absolute)),
                    "p99_absolute_component_px": float(np.quantile(absolute, .99)),
                    "p999_absolute_component_px": float(np.quantile(absolute, .999)),
                    "maximum_radial_offset_px": float(np.max(radial)),
                    "p99_radial_offset_px": float(np.quantile(radial, .99)),
                    "p999_radial_offset_px": float(np.quantile(radial, .999)),
                    "offset_shape": list(offsets.shape),
                })
    finally:
        handle.remove()
    report = {
        "schemaVersion": 1,
        "status": "completed_cpu_offset_probe",
        "source": str(args.source.resolve()),
        "checkpoint": str(args.checkpoint.resolve()),
        "checkpoint_sha256": sha(args.checkpoint),
        "shape": [1, 7, args.height, args.width],
        "cases": cases,
        "interpretation": [
            "Offsets are measured on full-frame generated windows only",
            "Offset statistics do not prove a safe tile halo or bound the U-Net receptive field",
            "Tile inference recomputes the offset head and must be qualified against whole-frame output",
        ],
    }
    (args.output / "offset-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--height", type=int, default=128)
    parser.add_argument("--width", type=int, default=192)
    args = parser.parse_args()
    if args.height % 8 or args.width % 8:
        raise ValueError("height and width must be divisible by 8")
    report = run(args)
    print(json.dumps({"status": report["status"], "report": str(args.output / "offset-report.json")}))


if __name__ == "__main__":
    main()

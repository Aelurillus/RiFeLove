"""CPU-only STDF-R3 feasibility export; never a native/redistributable model pack.

The author-maintained Apache-2.0 network is imported from a clean, pinned local
checkout. Its CUDA-only DCNv2 wrapper is replaced with independently implemented
GridSample + 1x1 convolution, checked against torchvision's CPU deform_conv2d.
Python remains a developer tool. No acquisition, uploads or user video access.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time
import types

import numpy as np
import torch
import torch.nn.functional as F

UPSTREAM = "https://github.com/ryanxingql/stdf-pytorch"
COMMIT = "8bedc89e840117f2634cf5183a7eb9441c302360"
ASSET = UPSTREAM + "/releases/download/v1.0.0/exp.zip"
ASSET_SHA256 = "566a7cce34a587c14fd0b413ba816f2a42f1ea63d6edda3ac2c525d8d25feb88"
CHECKPOINT_SHA256 = "d28e0b30082ecbbaeb7b8436968079875c97d882101cc4a79114d33eca2e1ec7"
OPTIONS = {
    "radius": 3,
    "stdf": {"in_nc": 1, "out_nc": 64, "nf": 32, "nb": 3,
             "base_ks": 3, "deform_ks": 3},
    "qenet": {"in_nc": 64, "out_nc": 1, "nf": 48, "nb": 8, "base_ks": 3},
}


def sha(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def portable_deform_conv(x, offset, mask, weight, bias, *, deformable_groups):
    """Stride-1 DCNv2 with odd square kernel and same-sized zero padding.

    Offset channels are (group, kernel_y, kernel_x, y/x), interleaved dy/dx.
    The implementation intentionally supports only the STDF contract: convolution
    groups=1, stride=dilation=1. It is not a general DCNv2 replacement.
    """
    batch, channels, height, width = x.shape
    # Kernel geometry is a model constant; only image dimensions are dynamic.
    kernel = int(weight.shape[2])
    per_group = channels // deformable_groups
    yy = torch.arange(height, device=x.device, dtype=x.dtype).reshape(1, 1, height, 1)
    xx = torch.arange(width, device=x.device, dtype=x.dtype).reshape(1, 1, 1, width)
    offsets = offset.reshape(batch, deformable_groups, kernel * kernel, 2, height, width)
    masks = mask.reshape(batch, deformable_groups, kernel * kernel, height, width)
    grouped = x.reshape(batch * deformable_groups, per_group, height, width)
    samples = []
    for point in range(kernel * kernel):
        row, column = divmod(point, kernel)
        sample_y = yy + row - kernel // 2 + offsets[:, :, point, 0]
        sample_x = xx + column - kernel // 2 + offsets[:, :, point, 1]
        grid = torch.stack((2 * (sample_x + .5) / width - 1,
                            2 * (sample_y + .5) / height - 1), dim=-1)
        sampled = F.grid_sample(grouped, grid.reshape(batch * deformable_groups, height, width, 2),
                                mode="bilinear", padding_mode="zeros", align_corners=False)
        sampled = sampled.reshape(batch, deformable_groups, per_group, height, width)
        samples.append((sampled * masks[:, :, point, None]).reshape(batch, channels, height, width))
    packed = torch.stack(samples, dim=2).reshape(batch, channels * kernel * kernel, height, width)
    return F.conv2d(packed, weight.reshape(weight.shape[0], -1, 1, 1), bias)


class PortableDeformConv(torch.nn.Module):
    """State-dict-compatible adapter; source topology and weights stay unchanged."""

    reference = False

    def __init__(self, in_channels, out_channels, kernel_size, stride=1, padding=0,
                 dilation=1, groups=1, deformable_groups=1, bias=True):
        super().__init__()
        if (stride, dilation, groups, padding) != (1, 1, 1, kernel_size // 2):
            raise ValueError("This feasibility adapter only supports STDF's stride-1 same-padding operator")
        if kernel_size % 2 != 1 or in_channels % deformable_groups:
            raise ValueError("Invalid STDF deformable-convolution geometry")
        self.deformable_groups = deformable_groups
        self.padding = padding
        self.weight = torch.nn.Parameter(torch.empty(out_channels, in_channels, kernel_size, kernel_size))
        self.bias = torch.nn.Parameter(torch.empty(out_channels)) if bias else None

    def forward(self, x, offset, mask):
        if self.reference:
            from torchvision.ops import deform_conv2d
            return deform_conv2d(x, offset, self.weight, self.bias, padding=self.padding, mask=mask)
        return portable_deform_conv(x, offset, mask, self.weight, self.bias,
                                    deformable_groups=self.deformable_groups)


def load_source_model(source: Path, checkpoint: Path):
    source = source.resolve()
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True).strip()
    if head != COMMIT or dirty:
        raise RuntimeError("A clean pinned upstream checkout is required")
    if sha(checkpoint) != CHECKPOINT_SHA256:
        raise RuntimeError("Checkpoint does not match the reviewed upstream MFQEv2 R3 release")
    # Scope the replacement only to this module load; no upstream source is edited.
    key = "ops.dcn.deform_conv"
    replacement = types.ModuleType(key)
    replacement.ModulatedDeformConv = PortableDeformConv
    previous = sys.modules.get(key)
    sys.modules[key] = replacement
    try:
        spec = importlib.util.spec_from_file_location("rifelove_stdf_pinned", source / "net_stdf.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        if previous is None:
            sys.modules.pop(key, None)
        else:
            sys.modules[key] = previous
    model = module.MFVQE(OPTIONS).cpu().eval()
    # Deserialization is restricted to tensor state; never permit arbitrary pickle code.
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict({key.removeprefix("module."): value
                           for key, value in state["state_dict"].items()}, strict=True)
    return model


def operator_cases():
    """Independent torchvision reference, including grouped and border samples."""
    from torchvision.ops import deform_conv2d
    generator = torch.Generator().manual_seed(876051)
    cases = []
    with torch.inference_mode():
        for batch, channels, groups, h, w in [(1, 7, 7, 8, 12), (2, 4, 2, 9, 11), (1, 1, 1, 1, 5)]:
            x = torch.rand(batch, channels, h, w, generator=generator)
            weight = torch.randn(3, channels, 3, 3, generator=generator) * .1
            bias = torch.randn(3, generator=generator) * .1
            for extent in [0., .7, 9.]:
                offset = torch.randn(batch, groups * 18, h, w, generator=generator) * extent
                mask = torch.rand(batch, groups * 9, h, w, generator=generator)
                expected = deform_conv2d(x, offset, weight, bias, padding=1, mask=mask)
                actual = portable_deform_conv(x, offset, mask, weight, bias, deformable_groups=groups)
                error = float(torch.max(torch.abs(expected - actual)))
                cases.append({"batch": batch, "channels": channels, "deformable_groups": groups,
                              "height": h, "width": w, "offset_std": extent,
                              "maximum_absolute_error": error, "passed": error < 1e-5})
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory; never overwrite previous artifacts")
    args.output.mkdir(parents=True)
    torch.set_num_threads(4)
    operator = operator_cases()
    if not all(case["passed"] for case in operator):
        raise RuntimeError("Portable operator differs from the independent torchvision reference")
    model = load_source_model(args.source, args.checkpoint)
    import onnx
    import onnxruntime as ort
    graph = args.output / "stdf-r3-y7-fp32.onnx"
    example = torch.full((1, 7, 64, 64), .5)
    torch.onnx.export(model, (example,), str(graph), opset_version=17, dynamo=False,
                      input_names=["luma_frames"], output_names=["restored_luma"],
                      dynamic_axes={"luma_frames": {2: "height", 3: "width"},
                                    "restored_luma": {2: "height", 3: "width"}})
    onnx.checker.check_model(str(graph))
    options = ort.SessionOptions(); options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(graph), sess_options=options, providers=["CPUExecutionProvider"])
    cases = []
    rng = np.random.default_rng(876051)
    with torch.inference_mode():
        for height, width in [(64, 64), (64, 96), (128, 192)]:
            frames = rng.uniform(.05, .95, (1, 7, height, width)).astype(np.float32)
            started = time.perf_counter()
            model.ffnet.deform_conv.reference = True
            reference = model(torch.from_numpy(frames)).numpy()
            model.ffnet.deform_conv.reference = False
            portable = model(torch.from_numpy(frames)).numpy()
            actual = session.run(None, {"luma_frames": frames})[0]
            portable_error = float(np.max(np.abs(reference - portable)))
            ort_error = float(np.max(np.abs(reference - actual)))
            cases.append({"input_shape": list(frames.shape), "output_shape": list(actual.shape),
                          "portable_vs_torchvision_max_error": portable_error,
                          "onnx_vs_torchvision_max_error": ort_error,
                          "passed": portable_error < 2e-5 and ort_error < 2e-5,
                          "seconds": time.perf_counter() - started})
    report = {"schemaVersion": 1, "status": "passed_cpu_feasibility" if all(c["passed"] for c in cases) else "failed",
              "graph_sha256": sha(graph), "checkpoint_sha256": sha(args.checkpoint),
              "source_repository": UPSTREAM, "source_commit": COMMIT, "checkpoint_release_asset": ASSET,
              "release_asset_sha256": ASSET_SHA256,
              "code_license": "Apache-2.0", "weight_redistribution_status": "unresolved; evaluation only",
              "source_hashes": {name: sha(args.source / name) for name in
                                ["LICENSE", "README.md", "net_stdf.py", "option_R3_mfqev2_1G.yml", "test_one_video.py",
                                 "ops/dcn/src/deform_conv_cuda_kernel.cu"]},
              "interface": {"input": "FP32 [1,7,H,W], ordered Y samples / 255, H/W divisible by 8",
                            "output": "FP32 [1,1,H,W], center-frame Y; unclamped upstream residual",
                            "color": "Raw encoded Y code values; RGB/range/UV integration remains unqualified",
                            "temporal_radius": 3, "native_adapter_registered": False},
              "operator_reference": "torchvision CPU deform_conv2d; original CUDA extension not executed",
              "operator_cases": operator, "model_cases": cases,
              "limitations": ["CPU/export feasibility only; no native Windows GPU support claimed",
                              "No redistribution approval or automatic strength estimator",
                              "Single training domain: HM16.5 HEVC LDP QP37; not generic video restoration",
                              "No product visual/performance/temporal or real-video qualification"],
              "tool_versions": {"torch": torch.__version__, "onnx": onnx.__version__, "onnxruntime": ort.__version__}}
    (args.output / "feasibility.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if report["status"] == "failed":
        raise RuntimeError("Export did not match the independent CPU operator reference")


if __name__ == "__main__":
    main()

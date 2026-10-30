"""Export the official five-frame FastDVDnet checkpoint for local evaluation.

Python is a development tool only. The exported C++ adapter runs without it.
No user media or credentials are read. Do not treat CPU equivalence as perceptual
qualification or as permission to redistribute weights in a product installer.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import time

import numpy as np
import onnx
import onnxruntime as ort
import torch

UPSTREAM = "https://github.com/m-tassano/fastdvdnet"
COMMIT = "c8fdf6182a0340e89dd18f5df25b47337cbede6f"
ADAPTER = {"id": "fastdvdnet-rgb-f32-nchw16-v1", "version": 1}
CAPABILITIES = {
    "tasks": ["denoise"], "placements": ["source-rate-before-interpolation"],
    "supportedInputProfiles": ["bt709-sdr8"], "temporalFramesBefore": 2,
    "temporalFramesAfter": 2, "scaleNumerator": 1, "scaleDenominator": 1,
    "supportsBoundaryPadding": True,
}


def sha(path: Path) -> str:
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()


def write(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if head != COMMIT or subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True).strip():
        raise RuntimeError("Export requires the clean pinned upstream source checkout")
    if output.exists():
        raise RuntimeError("Refusing to replace an existing evaluation model pack")
    output.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location("rifelove_fastdvdnet_source", source / "models.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    model = module.FastDVDnet().cpu().eval()
    checkpoint = source / "model_clipped_noise.pth"
    weights = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model.load_state_dict({key.removeprefix("module."): value for key, value in weights.items()}, strict=True)
    torch.set_num_threads(4)

    class PackedModel(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.temporal = model

        def forward(self, frames_and_noise):
            return torch.clamp(self.temporal(frames_and_noise[:, :15], frames_and_noise[:, 15:16]), 0., 1.)

    exported = PackedModel().eval()
    graph = output / "model.onnx"
    example = torch.full((1, 16, 64, 64), .25)
    torch.onnx.export(exported, (example,), str(graph), input_names=["frames_and_noise"],
                      output_names=["restored"], opset_version=17, dynamo=False,
                      dynamic_axes={"frames_and_noise": {2: "height", 3: "width"},
                                    "restored": {2: "height", 3: "width"}})
    onnx.checker.check_model(str(graph))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    session = ort.InferenceSession(str(graph), sess_options=options, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(714031)
    cases = []
    with torch.inference_mode():
        for height, width in [(64, 64), (64, 128), (192, 320)]:
            frames = rng.uniform(.1, .9, (1, 16, height, width)).astype(np.float32)
            frames[:, 15] = 25. / 255.
            started = time.perf_counter()
            reference = exported(torch.from_numpy(frames)).numpy()
            actual = session.run(None, {"frames_and_noise": frames})[0]
            error = float(np.max(np.abs(reference - actual)))
            mse = float(np.mean(np.square(reference.astype(np.float64) - actual)))
            cases.append({"case_id": f"five-frames-{width}x{height}", "passed": error < 2e-5,
                          "input_shape": list(frames.shape), "output_shape": list(actual.shape),
                          "maximum_absolute_error": error, "psnr_db": float(-10 * np.log10(max(mse, 1e-30))),
                          "seconds": time.perf_counter() - started})
    if not all(case["passed"] for case in cases):
        raise RuntimeError("The exported model did not match PyTorch")
    (output / "LICENSE.txt").write_bytes((source / "LICENSE").read_bytes())
    write(output / "source.json", {
        "upstreamRepository": UPSTREAM, "upstreamCommit": COMMIT, "license": "MIT",
        "licenseScopeEvidence": "Repository root LICENSE and README state MIT. Both checkpoints are tracked in that same pinned repository; no separate checkpoint restriction found. Training-data rights and final redistribution remain separately unqualified.",
        "files": {name: sha(source / name) for name in ["LICENSE", "README.md", "models.py", "model_clipped_noise.pth"]},
        "trainingData": "Upstream README states DAVIS 2017; RifeLove has not independently qualified dataset rights.",
        "checkpoint": "model_clipped_noise.pth", "parameter_count": sum(p.numel() for p in model.parameters()),
    })
    backends = ["pytorch_cpu_source", "onnxruntime_cpu_derived"]
    profile = "sdr8-fp32-pytorch-reference"
    write(output / "validation.json", {
        "status": "passed_cpu_reference", "model_sha256": sha(graph), "model_adapter": ADAPTER,
        "numerical_profile": profile, "validated_backends": backends,
        "capabilities": CAPABILITIES, "cases": cases,
        "limitations": "Local five-frame Gaussian-denoise experiment only. No compression repair, detail reconstruction, super-resolution, HDR, real-video temporal-quality or distribution qualification.",
    })
    write(output / "model.json", {
        "schemaVersion": 1, "family": "video-restoration", "id": "fastdvdnet-clipped-five-frame-sdr8-eval",
        "displayName": "FastDVDnet five-frame Gaussian denoise (local evaluation)", "version": "2026.10.28",
        "modelFile": graph.name, "sha256": sha(graph), "adapter": ADAPTER,
        "license": {"id": "MIT", "file": "LICENSE.txt", "sha256": sha(output / "LICENSE.txt")},
        "provenance": {"manifestFile": "source.json", "manifestSha256": sha(output / "source.json")},
        "capabilities": CAPABILITIES,
        "validation": {"reportFile": "validation.json", "reportSha256": sha(output / "validation.json"),
                       "numericalProfile": profile, "validatedBackends": backends},
    })
    print(json.dumps({"pack": str(output), "graph_sha256": sha(graph), "cases": cases}, indent=2))


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: Apache-2.0
"""Export a local Real-ESRGAN x2plus evaluation pack for RifeLove.

The RRDBNet layout follows BasicSR's Apache-2.0 implementation. The published
checkpoint is used only for local evaluation until its separate weight-license
scope is established; the generated pack must not be redistributed.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import onnx
import onnxruntime as ort
import torch
from torch import nn
from torch.nn import functional as functional

from realesrgan_x4plus_export import RRDB, sha256_file, write_json


UPSTREAM_REPOSITORY = "https://github.com/xinntao/Real-ESRGAN"
UPSTREAM_RELEASE = "v0.2.1"
UPSTREAM_COMMIT = "64ad194ddaf9c4d8c4b0d1b98cac6d89d3ea0d11"
WEIGHT_URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth"
WEIGHT_SCOPE_EVIDENCE = (
    "The upstream Real-ESRGAN source repository is BSD-3-Clause and its model zoo links this checkpoint as a release asset, "
    "but RifeLove has not established whether the repository license explicitly covers redistribution of this separate weight asset. "
    "See https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE, "
    "https://github.com/xinntao/Real-ESRGAN/blob/master/docs/model_zoo.md, and "
    "https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.1. Local evaluation only; do not redistribute this model pack."
)
ADAPTER_ID = "image-sr-rgb-f32-nchw-v1"


class RRDBNetX2(nn.Module):
    """BasicSR RRDBNet scale=2: pixel-unshuffle input then two x2 upsamplers."""

    def __init__(self) -> None:
        super().__init__()
        self.conv_first = nn.Conv2d(12, 64, 3, 1, 1)
        self.body = nn.Sequential(*(RRDB() for _ in range(23)))
        self.conv_body = nn.Conv2d(64, 64, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(64, 64, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(64, 64, 3, 1, 1)
        self.conv_hr = nn.Conv2d(64, 64, 3, 1, 1)
        self.conv_last = nn.Conv2d(64, 3, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=False)

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        features = self.conv_first(functional.pixel_unshuffle(value, 2))
        body = self.conv_body(self.body(features))
        features = features + body
        features = self.lrelu(self.conv_up1(functional.interpolate(features, scale_factor=2, mode="nearest")))
        features = self.lrelu(self.conv_up2(functional.interpolate(features, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(features)))


def pack_capabilities() -> dict[str, object]:
    return {
        "tasks": ["super-resolution"],
        "placements": ["output-rate-after-interpolation"],
        "supportedInputProfiles": ["bt709-sdr8", "bt470bg-sdr8"],
        "temporalFramesBefore": 0,
        "temporalFramesAfter": 0,
        "scaleNumerator": 2,
        "scaleDenominator": 1,
        "supportsBoundaryPadding": False,
    }


def write_pack_license(path: Path) -> None:
    text = (
        "The upstream Real-ESRGAN source code is BSD-3-Clause. This does not establish the license for the separate pretrained weights.\n"
        "This local evaluation pack is not cleared for redistribution.\n\n"
        "BSD 3-Clause License\n\n"
        "Copyright (c) 2021, Xintao Wang\n"
        "All rights reserved.\n\n"
        "Redistribution and use in source and binary forms, with or without\n"
        "modification, are permitted provided that the following conditions are met:\n\n"
        "1. Redistributions of source code must retain the above copyright notice, this\n"
        "   list of conditions and the following disclaimer.\n"
        "2. Redistributions in binary form must reproduce the above copyright notice,\n"
        "   this list of conditions and the following disclaimer in the documentation\n"
        "   and/or other materials provided with the distribution.\n"
        "3. Neither the name of the copyright holder nor the names of its contributors\n"
        "   may be used to endorse or promote products derived from this software\n"
        "   without specific prior written permission.\n\n"
        "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
        "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
        "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE\n"
        "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
        "FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
        "DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
        "SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
        "CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR\n"
        "TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
        "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    )
    path.write_text(text, encoding="utf-8")


def export_pack(weights_path: Path, output_directory: Path) -> None:
    if not weights_path.is_file():
        raise FileNotFoundError(f"Checkpoint not found: {weights_path}")
    if output_directory.exists() and any(output_directory.iterdir()):
        raise FileExistsError(f"Output directory is not empty: {output_directory}")
    output_directory.mkdir(parents=True, exist_ok=True)

    checkpoint = torch.load(weights_path, map_location="cpu", weights_only=True)
    state = checkpoint.get("params_ema")
    if not isinstance(state, dict):
        raise ValueError("Checkpoint does not contain the expected params_ema state dictionary")
    model = RRDBNetX2().eval()
    model.load_state_dict(state, strict=True)

    graph_path = output_directory / "model.onnx"
    example = torch.linspace(0.0, 1.0, 3 * 64 * 64, dtype=torch.float32).reshape(1, 3, 64, 64)
    with torch.inference_mode():
        torch.onnx.export(
            model,
            example,
            graph_path,
            input_names=["input"],
            output_names=["output"],
            dynamic_axes={"input": {2: "input_height", 3: "input_width"}, "output": {2: "output_height", 3: "output_width"}},
            opset_version=17,
            do_constant_folding=True,
            dynamo=False,
        )
    graph = onnx.load(str(graph_path))
    onnx.checker.check_model(graph)

    session = ort.InferenceSession(str(graph_path), providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise RuntimeError("CPU ONNX validation unexpectedly selected a different provider")
    cases = []
    for height, width, seed in ((64, 64, 17), (64, 128, 23)):
        generator = torch.Generator(device="cpu").manual_seed(seed)
        sample = torch.rand((1, 3, height, width), generator=generator, dtype=torch.float32)
        with torch.inference_mode():
            reference = model(sample).clamp(0.0, 1.0).cpu().numpy()
        actual = session.run(["output"], {"input": sample.numpy()})[0]
        actual = actual.clip(0.0, 1.0)
        if actual.shape != reference.shape or actual.shape != (1, 3, height * 2, width * 2):
            raise RuntimeError(f"ONNX output shape mismatch for {height}x{width}: {actual.shape}")
        difference = abs(reference - actual)
        max_error = float(difference.max())
        mean_error = float(difference.mean())
        if not math.isfinite(max_error) or max_error > 0.003:
            raise RuntimeError(f"CPU ONNX numerical validation failed for {height}x{width}: max error {max_error}")
        cases.append({
            "case_id": f"h{height}-w{width}",
            "input_shape": [1, 3, height, width],
            "output_shape": list(actual.shape),
            "max_abs_error": max_error,
            "mean_abs_error": mean_error,
            "passed": True,
        })

    graph_hash = sha256_file(graph_path)
    write_pack_license(output_directory / "LICENSE.txt")
    license_hash = sha256_file(output_directory / "LICENSE.txt")
    source_manifest = {
        "schemaVersion": 1,
        "modelId": "realesrgan-x2plus-sdr8-local",
        "upstreamRepository": UPSTREAM_REPOSITORY,
        "upstreamRelease": UPSTREAM_RELEASE,
        "upstreamCommit": UPSTREAM_COMMIT,
        "license": "BSD-3-Clause",
        "licenseScopeEvidence": WEIGHT_SCOPE_EVIDENCE,
        "weightSourceUrl": WEIGHT_URL,
        "weightSha256": sha256_file(weights_path),
        "weightLicenseStatus": "unconfirmed; local evaluation only; do not redistribute",
        "architectureSource": "https://github.com/XPixelGroup/BasicSR/blob/master/basicsr/archs/rrdbnet_arch.py",
        "architectureLicense": "Apache-2.0; see tools/model_export/licenses/LICENSE-Apache-2.0.txt",
    }
    write_json(output_directory / "source-manifest.json", source_manifest)
    source_hash = sha256_file(output_directory / "source-manifest.json")
    capabilities = pack_capabilities()
    numerical_profile = "sdr8-rgb-f32-nchw-x2-v1"
    backends = ["pytorch_cpu", "onnxruntime_cpu"]
    validation = {
        "status": "passed_cpu_reference",
        "model_sha256": graph_hash,
        "model_adapter": {"id": ADAPTER_ID, "version": 1},
        "numerical_profile": numerical_profile,
        "validated_backends": backends,
        "capabilities": capabilities,
        "providers": session.get_providers(),
        "cases": cases,
    }
    write_json(output_directory / "validation.json", validation)
    validation_hash = sha256_file(output_directory / "validation.json")
    descriptor = {
        "schemaVersion": 1,
        "id": "realesrgan-x2plus-sdr8-local",
        "displayName": "Real-ESRGAN x2plus (local evaluation)",
        "family": "video-restoration",
        "version": "v0.2.1-onnx1",
        "adapter": {"id": ADAPTER_ID, "version": 1},
        "modelFile": "model.onnx",
        "sha256": graph_hash,
        "license": {"id": "BSD-3-Clause", "file": "LICENSE.txt", "sha256": license_hash},
        "provenance": {"manifestFile": "source-manifest.json", "manifestSha256": source_hash},
        "capabilities": capabilities,
        "validation": {
            "reportFile": "validation.json",
            "reportSha256": validation_hash,
            "numericalProfile": numerical_profile,
            "validatedBackends": backends,
        },
    }
    write_json(output_directory / "model.json", descriptor)
    print(json.dumps({
        "pack": str(output_directory.resolve()),
        "graph_sha256": graph_hash,
        "weight_sha256": source_manifest["weightSha256"],
        "cases": cases,
        "weight_redistribution": "unconfirmed; local evaluation only",
    }, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", type=Path, required=True, help="Local official RealESRGAN_x2plus.pth checkpoint")
    parser.add_argument("--output", type=Path, required=True, help="New local restoration model-pack directory")
    args = parser.parse_args()
    export_pack(args.weights, args.output)


if __name__ == "__main__":
    main()

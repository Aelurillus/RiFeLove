#!/usr/bin/env python3
"""Compare the FP32 NAFNet graph with its original and create local fixtures.

This checks conversion parity and records a reference for backend comparisons.
It is not a visual-quality, video-temporal, provenance, or redistribution
approval. Run nafnet_zoo_dequantize.py to create the destination first.
"""

import argparse
import hashlib
import json
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort

GRAPH = "model.onnx"
SOURCE_SHA256 = "07263f416febeccE10193dd648e950b22e397cf521eedab1a114ef77b2bc9587".lower()
ADAPTER = "nafnet-zoo-deblur-rgb-f32-nchw-v1"
MODEL_ID = "nafnet-zoo-gopro-sdr8-local"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_new(path, data):
    with path.open("xb") as stream:
        stream.write(data)


def write_json(path, value):
    write_new(path, (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def make_input(height, width):
    y, x = np.mgrid[:height, :width]
    rng = np.random.default_rng(height * 10000 + width)
    planes = [0.45 + 0.24 * np.sin(x / 23) + 0.16 * np.cos(y / 17),
              0.50 + 0.20 * np.cos((x + y) / 29),
              0.45 + 0.20 * np.sin(y / 31) + 0.10 * np.cos(x / 11)]
    image = np.stack(planes, axis=-1) + rng.normal(0, 0.025, (height, width, 3))
    return np.floor(np.clip(image, 0, 1) * 255 + 0.5).astype(np.uint8)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path, help="Existing ignored local candidate directory")
    parser.add_argument("source", type=Path, help="Original downloaded Zoo ONNX graph")
    args = parser.parse_args()
    root = args.pack.resolve(strict=True)
    source = args.source.resolve(strict=True)
    graph, license_file = root / GRAPH, root / "LICENSE"
    conversion = json.loads((root / "conversion.json").read_text(encoding="utf-8"))
    graph_hash = sha256(graph)
    if sha256(source) != SOURCE_SHA256 or conversion.get("source_sha256") != SOURCE_SHA256 or conversion.get("derived_sha256") != graph_hash:
        raise SystemExit("Source or converted graph SHA-256 differs from the conversion report")
    if not license_file.is_file():
        raise SystemExit("The OpenCV Zoo LICENSE must be present")
    for filename in ("model.json", "source-manifest.json", "validation.json", "fixtures"):
        if (root / filename).exists():
            raise SystemExit(f"Refusing to overwrite {root / filename}")

    ort.set_default_logger_severity(4)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    options.log_severity_level = 4
    session = ort.InferenceSession(str(source), sess_options=options, providers=["CPUExecutionProvider"])
    converted = ort.InferenceSession(str(graph), sess_options=options, providers=["CPUExecutionProvider"])
    inputs, outputs = session.get_inputs(), session.get_outputs()
    if len(inputs) != 1 or len(outputs) != 1 or inputs[0].type != "tensor(float)" or outputs[0].type != "tensor(float)":
        raise SystemExit("Unexpected ONNX tensor contract")

    fixture_dir = root / "fixtures"
    fixture_dir.mkdir()
    cases = []
    for height, width in ((384, 384), (128, 192), (384, 512)):
        image = make_input(height, width)
        tensor = np.transpose(image.astype(np.float32) / 255.0, (2, 0, 1))[None]
        tensor = np.pad(tensor, ((0, 0), (0, 0), (0, max(0, 384 - height)), (0, max(0, 384 - width))), mode="edge")
        started = time.perf_counter()
        result = session.run(None, {inputs[0].name: tensor})[0]
        actual = converted.run(None, {converted.get_inputs()[0].name: tensor})[0]
        elapsed = time.perf_counter() - started
        if result.shape != tensor.shape or actual.shape != result.shape or not np.isfinite(result).all() or not np.isfinite(actual).all():
            raise RuntimeError(f"Unexpected output for {width}x{height}: {result.shape}")
        difference = np.abs(result - actual).astype(np.float64)
        max_error = float(difference.max())
        mse = float(np.mean(difference * difference))
        psnr = 999.0 if mse == 0 else float(-10 * np.log10(mse))
        if max_error > 0.5 / 255 or psnr < 80:
            raise RuntimeError(f"Converted ONNX differs from the original for {width}x{height}: max={max_error}, PSNR={psnr}")
        output = np.transpose(result[0, :, :height, :width], (1, 2, 0))
        output = np.floor(np.clip(output, 0, 1) * 255 + 0.5).astype(np.uint8)
        input_file = fixture_dir / f"input-{width}x{height}.rgb"
        output_file = fixture_dir / f"cpu-output-{width}x{height}.rgb"
        write_new(input_file, image.tobytes())
        write_new(output_file, output.tobytes())
        cases.append({"case_id": f"rgb-{width}x{height}", "input_shape": list(tensor.shape),
                      "output_shape": list(result.shape), "input_file": input_file.name,
                      "input_sha256": sha256(input_file), "cpu_output_file": output_file.name,
                      "cpu_output_sha256": sha256(output_file), "max_abs_error": max_error,
                      "psnr_db": psnr, "seconds": elapsed, "passed": True})

    capabilities = {"tasks": ["deblur"], "placements": ["output-rate-after-interpolation"],
                    "supportedInputProfiles": ["bt709-sdr8"], "temporalFramesBefore": 0,
                    "temporalFramesAfter": 0, "scaleNumerator": 1, "scaleDenominator": 1,
                    "supportsBoundaryPadding": False}
    manifest = {"schemaVersion": 1, "modelId": MODEL_ID,
                "upstreamRepository": "https://github.com/opencv/opencv_zoo",
                "upstreamCommit": "unrecorded-linked-2025may-onnx", "license": "MIT",
                "licenseScopeEvidence": "https://github.com/opencv/opencv_zoo/tree/main/models/deblurring_nafnet",
                "weightLicenseStatus": "Linked ONNX weight redistribution is unconfirmed; local evaluation only.",
                "sourceGraphSha256": SOURCE_SHA256, "derivedGraphSha256": graph_hash,
                "conversionReportSha256": sha256(root / "conversion.json")}
    write_json(root / "source-manifest.json", manifest)
    profile = "sdr8-fp32-cpu-reference"
    backends = ["onnxruntime_cpu_source", "onnxruntime_cpu_derived"]
    report = {"status": "passed_cpu_reference", "model_sha256": graph_hash,
              "model_adapter": {"id": ADAPTER, "version": 1}, "numerical_profile": profile,
              "validated_backends": backends, "capabilities": capabilities, "cases": cases,
              "limitations": "Compares the derived graph with the downloaded ONNX only; no PyTorch, video quality or license qualification."}
    write_json(root / "validation.json", report)
    descriptor = {"schemaVersion": 1, "id": MODEL_ID, "displayName": "NAFNet GoPro (local evaluation)",
                  "family": "video-restoration", "version": "2025may-fp32-local1",
                  "adapter": {"id": ADAPTER, "version": 1}, "modelFile": GRAPH, "sha256": graph_hash,
                  "license": {"id": "MIT", "file": "LICENSE", "sha256": sha256(license_file)},
                  "provenance": {"manifestFile": "source-manifest.json", "manifestSha256": sha256(root / "source-manifest.json")},
                  "capabilities": capabilities,
                  "validation": {"reportFile": "validation.json", "reportSha256": sha256(root / "validation.json"),
                                 "numericalProfile": profile, "validatedBackends": backends}}
    write_json(root / "model.json", descriptor)
    print(json.dumps({"pack": str(root), "cases": cases, "distribution": "local evaluation only"}, indent=2))


if __name__ == "__main__":
    main()

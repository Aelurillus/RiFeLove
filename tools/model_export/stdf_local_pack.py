"""Build a *local evaluation only* STDF pack from already verified evidence.

This developer tool does not download models, infer a weight license, or create
a distributable package. The desktop runtime itself does not need Python.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path


GRAPH_SHA256 = "dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721"
SOURCE_COMMIT = "8bedc89e840117f2634cf5183a7eb9441c302360"
ADAPTER = {"id": "stdf-r3-y7-luma-v1", "version": 1}
PROFILE = "sdr8-fp32-stdf-r3-cpu-reference"
BACKENDS = ["onnxruntime_cpu_stdf_r3", "onnxruntime_cuda_stdf_r3"]
LICENSE_ID = "stdf-weights-unverified-local-evaluation"
CAPABILITIES = {
    "tasks": ["deblock"],
    "placements": ["source-rate-before-interpolation"],
    "supportedInputProfiles": ["bt709-sdr8"],
    "temporalFramesBefore": 3,
    "temporalFramesAfter": 3,
    "scaleNumerator": 1,
    "scaleDenominator": 1,
    "supportsBoundaryPadding": True,
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def document(path: Path) -> tuple[dict, str]:
    data = path.read_bytes()
    if not data or len(data) > 1024 * 1024:
        raise ValueError(f"Evidence must be non-empty and at most 1 MiB: {path}")
    result = json.loads(data)
    if not isinstance(result, dict):
        raise ValueError(f"Evidence must be a JSON object: {path}")
    return result, digest(data)


def require_cases(report: dict, key: str) -> list[dict]:
    cases = report.get(key)
    if not isinstance(cases, list) or not cases or any(not isinstance(case, dict) or case.get("passed") is not True for case in cases):
        raise ValueError(f"Evidence has missing or failed {key}")
    return cases


def bounded_number(value: object, upper: float) -> bool:
    return type(value) in (int, float) and math.isfinite(value) and 0 <= value <= upper


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def create_pack(graph: Path, cpu_evidence: Path, cuda_evidence: Path,
                native_evidence: Path, output: Path) -> None:
    if not graph.is_file() or graph.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("The local graph must be a regular ONNX file no larger than 2 MiB")
    graph_data = graph.read_bytes()
    if digest(graph_data) != GRAPH_SHA256:
        raise ValueError("The local graph does not match the qualified STDF graph SHA-256")
    cpu, cpu_hash = document(cpu_evidence)
    cuda, cuda_hash = document(cuda_evidence)
    native, native_hash = document(native_evidence)
    if (cpu.get("status") != "passed_cpu_feasibility" or cpu.get("graph_sha256") != GRAPH_SHA256
            or cpu.get("source_commit") != SOURCE_COMMIT):
        raise ValueError("CPU feasibility evidence does not match the reviewed graph and source")
    cpu_cases = require_cases(cpu, "model_cases")
    operator_cases = require_cases(cpu, "operator_cases")
    if (not all(bounded_number(case.get("onnx_vs_torchvision_max_error"), 3e-5) for case in cpu_cases)
            or not all(bounded_number(case.get("maximum_absolute_error"), 3e-5) for case in operator_cases)):
        raise ValueError("CPU evidence has an invalid or excessive reference error")
    if cuda.get("status") != "passed_native_cuda_boundary_checks" or cuda.get("graph_sha256") != GRAPH_SHA256:
        raise ValueError("CUDA parity evidence does not match the reviewed graph")
    cuda_cases = require_cases(cuda, "cases")
    required_cuda_shapes = {"cuda-64x64", "cuda-192x128", "cuda-384x256"}
    if (not required_cuda_shapes.issubset({case.get("name") for case in cuda_cases})
            or not all(bounded_number(case.get("maximum_float_error"), 3e-5)
                       and bounded_number(case.get("maximum_8bit_error_levels"), 1) for case in cuda_cases)):
        raise ValueError("CUDA evidence does not cover the required shapes and parity bounds")
    if (native.get("passed") is not True or native.get("backend") != "onnxruntime_cuda"
            or native.get("runtimeVersion") != "1.22.0"
            or native.get("cudaProviderAvailable") is not True or native.get("cudaSessionCreated") is not True
            or native.get("criticalComputeVerified") is not True or native.get("cpuImageFallbackDetected") is not False
            or type(native.get("cudaConvNodeCount")) is not int or native["cudaConvNodeCount"] < 20
            or type(native.get("cudaGridSampleNodeCount")) is not int or native["cudaGridSampleNodeCount"] < 9):
        raise ValueError("Native CUDA evidence does not prove the qualified provider assignment")
    license_info = (
        "Local evaluation only. Rights to redistribute the checkpoint and derived graph have not been established.\n"
        "The upstream source code declares Apache-2.0; that notice does not establish model-weight rights.\n"
        "Do not upload, mirror, package for distribution, or present this notice as a model license.\n"
    ).encode("utf-8")
    source = json_bytes({
        "license": LICENSE_ID,
        "upstreamRepository": "https://github.com/ryanxingql/stdf-pytorch",
        "upstreamCommit": SOURCE_COMMIT,
        "upstreamCodeLicense": "Apache-2.0",
        "licenseScopeEvidence": "Model-weight redistribution rights are unverified; this pack is local evaluation only.",
        "checkpointSha256": cpu.get("checkpoint_sha256"),
        "cpuEvidenceSha256": cpu_hash,
        "cudaEvidenceSha256": cuda_hash,
        "nativeEvidenceSha256": native_hash,
    })
    cases = ([{"name": f"cpu-model-{index}", "passed": True} for index, _ in enumerate(cpu_cases)]
             + [{"name": str(case.get("name", f"cuda-{index}")), "passed": True}
                for index, case in enumerate(cuda_cases)]
             + [{"name": "native-cuda-provider-assignment", "passed": True}])
    validation = json_bytes({
        "status": "passed_local_evaluation_only",
        "model_sha256": GRAPH_SHA256,
        "numerical_profile": PROFILE,
        "validated_backends": BACKENDS,
        "model_adapter": ADAPTER,
        "capabilities": CAPABILITIES,
        "cases": cases,
        "limitations": ["BT.709 limited-range 8-bit planar YUV only", "Maximum validated input is 384x256",
                        "No model redistribution approval", "Not a universal video quality guarantee"],
    })
    descriptor = json_bytes({
        "schemaVersion": 1,
        "family": "video-restoration",
        "id": "stdf-r3-y7-sdr8-local-eval",
        "displayName": "STDF-R3 temporal compression repair (local evaluation)",
        "version": "2026.10.31",
        "modelFile": "model.onnx",
        "sha256": GRAPH_SHA256,
        "adapter": ADAPTER,
        "license": {"id": LICENSE_ID, "file": "LICENSE-LOCAL-EVALUATION.txt", "sha256": digest(license_info)},
        "provenance": {"manifestFile": "source.json", "manifestSha256": digest(source)},
        "capabilities": CAPABILITIES,
        "validation": {"reportFile": "validation.json", "reportSha256": digest(validation),
                       "numericalProfile": PROFILE, "validatedBackends": BACKENDS},
    })
    # Validate everything before making a fresh directory. Never replace a
    # model pack the user may already be using.
    output.mkdir(parents=False, exist_ok=False)
    for name, payload in (("model.onnx", graph_data), ("LICENSE-LOCAL-EVALUATION.txt", license_info),
                          ("source.json", source), ("validation.json", validation), ("model.json", descriptor)):
        with (output / name).open("xb") as stream:
            stream.write(payload)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--cpu-evidence", type=Path, required=True)
    parser.add_argument("--cuda-evidence", type=Path, required=True)
    parser.add_argument("--native-evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    create_pack(args.graph, args.cpu_evidence, args.cuda_evidence, args.native_evidence, args.output)
    print(f"Created local-evaluation-only STDF pack: {args.output}")


if __name__ == "__main__":
    main()

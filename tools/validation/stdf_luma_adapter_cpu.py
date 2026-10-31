"""Real CPU ONNX round-trip through the isolated native seven-luma adapter.

No CUDA, application registration, packaging, uploads or production edits.
The native harness owns validation/lifetime tests; this independent packing and
quantization oracle checks its on-disk inputs and assembled Y/U/V outputs.
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

GRAPH_SHA256 = "dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721"


def verified_graph(path: Path) -> bytes:
    if not path.is_file() or not 0 < path.stat().st_size <= 2 * 1024 * 1024:
        raise ValueError("Graph must be a nonempty bounded regular local file")
    with path.open("rb") as source:
        contents = source.read(2 * 1024 * 1024 + 1)
    if len(contents) > 2 * 1024 * 1024:
        raise ValueError("Graph grew past the local byte limit while opening")
    if hashlib.sha256(contents).hexdigest() != GRAPH_SHA256:
        raise ValueError("Graph SHA-256 does not match the independently qualified weights")
    return contents


def packing_oracle(item: dict, directory: Path) -> np.ndarray:
    width, height = item["source_size"]
    shape = item["shape"]
    if shape[:2] != [1, 7] or shape[2:] != [(height + 7) // 8 * 8, (width + 7) // 8 * 8]:
        raise ValueError("Unexpected source/padded tensor geometry")
    channels = []
    for source_index in item["context_slots"]:
        plane = np.fromfile(directory / f"real-{source_index}-y.u8", dtype=np.uint8)
        if plane.size != width * height:
            raise ValueError("Real-frame plane length does not match source geometry")
        padded = np.pad(plane.reshape(height, width), ((0, shape[2] - height), (0, shape[3] - width)), mode="edge")
        channels.append(padded.astype(np.float32) / np.float32(255))
    return np.stack(channels, axis=0)[None]


def quantization_oracle(prediction: np.ndarray, original: np.ndarray, width: int, height: int,
                        strength: float = 1) -> np.ndarray:
    if prediction.ndim != 4 or prediction.shape[:2] != (1, 1) or prediction.shape[2] < height or prediction.shape[3] < width:
        raise ValueError("Unexpected prediction tensor shape")
    if original.size != width * height or not np.isfinite(prediction).all() or not np.isfinite(strength) or not 0 <= strength <= 1:
        raise ValueError("Invalid prediction, strength or center luma")
    # Match the documented float32 encoded-code blend, positive std::lround and
    # explicit crop. Never interpret limited-range code values as linear light.
    decoded = np.clip(prediction[0, 0, :height, :width], np.float32(0), np.float32(1)) * np.float32(255)
    center = original.reshape(height, width).astype(np.float32)
    blended = np.clip(center + np.float32(strength) * (decoded - center), np.float32(0), np.float32(255))
    return np.floor(blended + np.float32(0.5)).astype(np.uint8)


def native(probe: Path, mode: str, graph: Path, output: Path) -> dict:
    result = subprocess.run([str(probe.resolve()), mode, str(graph.resolve()), str(output.resolve())],
                            capture_output=True, text=True, timeout=120)
    (output.parent / f"{output.name}-{mode}-stdout.log").write_text(result.stdout, encoding="utf-8")
    (output.parent / f"{output.name}-{mode}-stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"Native {mode} failed: {result.stderr.strip()}")
    return json.loads((output / f"{mode}.json").read_text(encoding="utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    graph_bytes = verified_graph(args.graph)
    prepared = native(args.probe, "prepare", args.graph, args.output)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(graph_bytes, options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise RuntimeError("This validation must remain CPU-only")
    report = {"schemaVersion": 1, "graph_sha256": GRAPH_SHA256, "onnxruntime_version": ort.__version__,
              "actual_providers": session.get_providers(), "selftest_count": len(prepared["selftests"]),
              "cases": [], "limitations": [
                  "Independent synthetic planar fixtures; no decoder/encoder/production scheduling test",
                  "Conservative 384x256 maximum; odd-size CPU fixtures do not qualify larger GPU shapes",
                  "Payload budget excludes runtime activations, allocator/metadata overhead and hidden decoder storage",
                  "Scene bounds are supplied; this tool does not implement or qualify a scene detector",
                  "Weight redistribution rights unresolved; no packaging or upload"]}
    predictions = {}
    for item in prepared["cases"]:
        folder = args.output / item["name"]
        expected = packing_oracle(item, folder)
        actual = np.fromfile(folder / "input.f32", dtype=np.float32).reshape(item["shape"])
        if not np.array_equal(actual, expected):
            raise AssertionError(f"{item['name']}: native tensor differs from independent packing oracle")
        begin = time.perf_counter()
        prediction = session.run(None, {session.get_inputs()[0].name: actual})[0]
        elapsed = time.perf_counter() - begin
        if prediction.shape != (1, 1, item["shape"][2], item["shape"][3]) or prediction.dtype != np.float32 or not np.isfinite(prediction).all():
            raise AssertionError("Real CPU model returned invalid tensor")
        prediction.tofile(folder / "prediction.f32")
        predictions[item["name"]] = prediction
        report["cases"].append({"name": item["name"], "source_size": item["source_size"], "shape": item["shape"],
                                "native_pack_exact": True, "cpu_run_seconds": elapsed,
                                "owned_payload_bytes": item["owned_payload_bytes"],
                                "retained_plane_span_bytes": item["retained_plane_span_bytes"]})
    completed = native(args.probe, "complete", args.graph, args.output)
    for prepared_case, native_case, case in zip(prepared["cases"], completed["cases"], report["cases"], strict=True):
        assert prepared_case["name"] == native_case["name"] == case["name"]
        folder = args.output / case["name"]
        width, height = case["source_size"]
        original = np.fromfile(folder / "center-0.u8", dtype=np.uint8)
        expected = quantization_oracle(predictions[case["name"]], original, width, height)
        actual = np.fromfile(folder / "output-0.u8", dtype=np.uint8).reshape(height, width)
        if not np.array_equal(expected, actual):
            raise AssertionError("Native crop/quantization differs from independent CPU oracle")
        for p in [1, 2]:
            if (folder / f"center-{p}.u8").read_bytes() != (folder / f"output-{p}.u8").read_bytes():
                raise AssertionError("Center U/V was modified")
        if any(prepared_case[k] != native_case[k] for k in ["center_index", "center_pts", "center_duration", "timestamp_recovered"]):
            raise AssertionError("Center metadata changed between round-trip stages")
        case.update({"crop_quantization_exact": True, "center_chroma_bytes_exact": True,
                     "center_metadata_exact": native_case["center_metadata_exact"],
                     "real_prediction_discarded_after_cancel": native_case["real_prediction_discarded_after_cancel"],
                     "changed_y_samples": native_case["changed_y_samples"]})
    report["passed"] = True
    (args.output / "adapter-cpu.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

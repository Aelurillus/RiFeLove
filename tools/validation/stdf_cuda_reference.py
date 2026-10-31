"""Isolated native ORT CUDA qualification of a local STDF feasibility graph.

Developer utility only. Uses generated tensors, pinned existing runtime DLLs,
independent CPU reference, real node profiles and explicit error/cancel cases.
No uploads, model packaging or application changes.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import threading
import time

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "model_export"))
from stdf_r3_portable import load_source_model, sha


def profile_summary(path: Path):
    events = json.loads(path.read_text(encoding="utf-8"))
    counts, operations, durations = Counter(), defaultdict(Counter), Counter()
    cpu_shape_only = True
    for event in events:
        args = event.get("args", {})
        provider = args.get("provider")
        if event.get("cat") == "Node" and provider:
            op = args.get("op_name", "unknown")
            counts[provider] += 1
            operations[provider][op] += 1
            durations[provider] += event.get("dur", 0)
            if provider == "CPUExecutionProvider":
                if op not in {"Gather", "Unsqueeze", "Mul", "Concat", "Cast", "Div"}:
                    cpu_shape_only = False
                for typed in args.get("output_type_shape", []):
                    for dtype, shape in typed.items():
                        if math.prod(shape) > 8 or (dtype != "int64" and shape != []):
                            cpu_shape_only = False
    gpu_ops = operations.get("CUDAExecutionProvider", {})
    cpu_compute = {op: count for op, count in operations.get("CPUExecutionProvider", {}).items()
                   if "Conv" in op or op in {"GridSample", "Gemm", "MatMul"}}
    return {"provider_kernel_events": dict(counts),
            "operations": {provider: dict(ops) for provider, ops in operations.items()},
            "profile_kernel_duration_us": dict(durations),
            "critical_compute_is_cuda": bool(gpu_ops.get("GridSample", 0)) and
                                        any("Conv" in op for op in gpu_ops) and not cpu_compute,
            "cpu_only_scalar_shape_outputs": cpu_shape_only,
            "cpu_critical_compute": cpu_compute}


def device_memory():
    result = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                                     text=True, timeout=10)
    return int(result.splitlines()[0].strip()) * 1024 * 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise RuntimeError("Choose a fresh evidence directory")
    args.output.mkdir(parents=True)
    torch.set_num_threads(4)
    model = load_source_model(args.source, args.checkpoint)
    model.ffnet.deform_conv.reference = True
    driver = subprocess.check_output(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"], text=True).strip()
    report = {"schemaVersion": 1, "status": "running_native_cuda_checks",
              "device": driver, "graph_sha256": sha(args.graph), "probe_sha256": sha(args.probe),
              "runtime_hashes": {name: sha(args.runtime / name) for name in
                                 ["onnxruntime.dll", "onnxruntime_providers_cuda.dll", "cudart64_12.dll", "cudnn64_9.dll"]},
              "cases": [], "error_cases": [],
              "limits": {"maximum_float_error": 3e-5, "maximum_8bit_error_levels": 1,
                         "arena_budget_bytes": 1024 ** 3},
              "limitations": ["Generated CPU tensors, not real-video quality, RGB/color or timeline acceptance",
                              "Explicit CUDA EP only; no TensorRT registration or Python GPU wheel",
                              "CPU control nodes are reported, not hidden; convolutions/GridSample must execute on CUDA",
                              "Device memory is observed by sampling, not an exact per-process allocation peak",
                              "ORT termination may not preempt already-enqueued CUDA kernels; discard their results after a cancellation request",
                              "Unresolved weight redistribution; no app/model packaging or uploads"]}

    def save():
        (args.output / "cuda-reference.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def probe(name, h, w, data, mode="cuda", budget=1024 ** 3, repeats=4):
        tensor = args.output / f"{name}-input.f32"
        data.tofile(tensor)
        output = args.output / f"{name}-output.f32"
        result = args.output / f"{name}.json"
        command = [str(args.probe.resolve()), str(args.runtime.resolve()), str(args.graph.resolve()), str(tensor.resolve()),
                   str(h), str(w), str(output.resolve()), str(result.resolve()), mode, str(repeats), str(budget)]
        samples, sampling_errors = [], []
        stop = threading.Event()

        def monitor():
            while not stop.is_set():
                try:
                    samples.append(device_memory())
                except Exception as error:
                    sampling_errors.append(str(error))
                stop.wait(.1)

        monitor_thread = threading.Thread(target=monitor)
        monitor_thread.start()
        begin = time.perf_counter()
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=180)
        finally:
            stop.set(); monitor_thread.join()
        (args.output / f"{name}-stderr.txt").write_text(completed.stderr, encoding="utf-8")
        if not result.exists():
            raise RuntimeError(f"Native probe did not create evidence: {completed.stderr[:1000]}")
        native = json.loads(result.read_text(encoding="utf-8"))
        native.update({"command": command, "process_return_code": completed.returncode,
                       "process_elapsed_seconds": time.perf_counter() - begin,
                       "observed_device_memory_peak_bytes": max(samples) if samples else None,
                       "observed_device_memory_samples": len(samples), "memory_sampling_errors": sampling_errors,
                       "output_exists": output.exists()})
        if native.get("profile_file"):
            native["profile"] = profile_summary(Path(native["profile_file"]))
        return native, output

    rng = np.random.default_rng(431790)
    with torch.inference_mode():
        for h, w in [(64, 64), (128, 192), (256, 384)]:
            name = f"cuda-{w}x{h}"
            data = rng.uniform(.05, .95, (1, 7, h, w)).astype(np.float32)
            started = time.perf_counter()
            reference = model(torch.from_numpy(data)).numpy()
            cpu_seconds = time.perf_counter() - started
            native, output = probe(name, h, w, data)
            case = {"name": name, "native": native, "independent_torchvision_cpu_seconds": cpu_seconds}
            if native["status"] == "completed_probe_not_product_qualification":
                actual = np.fromfile(output, np.float32).reshape(reference.shape)
                delta = np.abs(actual.astype(np.float64) - reference)
                error = float(np.max(delta))
                quantized = int(np.max(np.abs(np.rint(np.clip(actual, 0, 1) * 255).astype(np.int16) -
                                                np.rint(np.clip(reference, 0, 1) * 255).astype(np.int16))))
                case.update({"maximum_float_error": error, "mean_float_error": float(np.mean(delta)),
                             "maximum_8bit_error_levels": quantized,
                             "passed": error < 3e-5 and quantized <= 1 and native["profile"]["critical_compute_is_cuda"]
                                       and native["profile"]["cpu_only_scalar_shape_outputs"]})
            else:
                case["passed"] = False
            report["cases"].append(case); save()
            print(json.dumps({"case": name, "passed": case["passed"], "error": case.get("maximum_float_error"),
                              "native_status": native["status"], "profile": native.get("profile")}), flush=True)

        for name, mode, h, w, budget in [("strict-cuda", "strict-cuda", 64, 64, 1024 ** 3),
                                        ("cancel", "cancel", 256, 384, 1024 ** 3),
                                        ("invalid-device", "invalid-device", 64, 64, 1024 ** 3),
                                        ("invalid-shape", "cuda", 65, 64, 1024 ** 3),
                                        ("arena-4mib", "cuda", 128, 192, 4 * 1024 ** 2)]:
            data = rng.uniform(.05, .95, (1, 7, h, w)).astype(np.float32)
            native, output = probe(name, h, w, data, mode, budget, 2)
            if name == "cancel" and native["status"] == "completed_probe_not_product_qualification":
                reference = model(torch.from_numpy(data)).numpy()
                actual = np.fromfile(output, np.float32).reshape(reference.shape)
                error = float(np.max(np.abs(actual.astype(np.float64) - reference)))
                cancellation = native.get("cancellation", {})
                passed = (cancellation.get("pre_cancel_rejected") and cancellation.get("request_observed_before_return") and
                          cancellation.get("in_flight_result_discarded") and not cancellation.get("cancelled_result_published") and
                          cancellation.get("subsequent_valid_run_succeeded") and error < 3e-5)
                native["post_cancel_maximum_float_error"] = error
            elif name == "strict-cuda":
                passed = (native["status"] == "ort_error" and not native["output_exists"] and
                          "fallback to CPU EP has been explicitly disabled" in native.get("error", ""))
                native["interpretation"] = "CPU fallback disabled reveals mandatory control-shape nodes; inspect error rather than claim all-GPU"
            else:
                expected = {"invalid-device": "Invalid device ID", "invalid-shape": "Non concat axis dimensions",
                            "arena-4mib": "smaller than requested bytes"}[name]
                passed = native["status"] == "ort_error" and not native["output_exists"] and expected in native.get("error", "")
            report["error_cases"].append({"name": name, "passed": bool(passed), "native": native}); save()
            print(json.dumps({"case": name, "passed": bool(passed), "status": native["status"],
                              "error": native.get("error"), "cancellation": native.get("cancellation")}), flush=True)
    report["status"] = "passed_native_cuda_boundary_checks" if all(c["passed"] for c in report["cases"] + report["error_cases"]) else "failed_native_cuda_boundary_checks"
    save()
    print(json.dumps({"status": report["status"], "report": str(args.output / "cuda-reference.json")}), flush=True)
    if report["status"].startswith("failed"):
        raise RuntimeError("Inspect native CUDA reference/boundary failures")


if __name__ == "__main__":
    main()

"""Export pinned RIFE 4.25 and validate dynamic ONNX output against upstream PyTorch."""

import argparse
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import types

from acquire_rife import ROOT, DEFAULT_MANIFEST, checked_bytes, digest


def verified_sources(directory, manifest):
    return {item["destination"]: checked_bytes(directory / item["destination"], item["sha256"])
            for item in [*manifest["archiveFiles"], *manifest["additionalFiles"]]}


def inference_state(state, inference_keys):
    inference_keys = set(inference_keys)
    training_keys = set(state) - inference_keys
    if inference_keys - set(state):
        raise ValueError("Checkpoint is missing inference parameters")
    if any(not key.startswith(("teacher.", "caltime.")) for key in training_keys):
        raise ValueError("Checkpoint contains unexpected non-training parameters")
    return {key: value for key, value in state.items() if key in inference_keys}


def load_network(sources):
    import torch
    warp_module = types.ModuleType("rifelove_pinned_warp")
    network_module = types.ModuleType("rifelove_pinned_ifnet_425")
    sys.modules[warp_module.__name__] = warp_module
    sys.modules[network_module.__name__] = network_module
    # Read and compile checksum-verified source bytes, never archived .pyc files.
    exec(compile(sources["warplayer.py"], "<pinned-rife-warplayer>", "exec"), warp_module.__dict__)
    previous_root = sys.modules.get("model")
    previous_warp = sys.modules.get("model.warplayer")
    sys.modules["model"] = types.ModuleType("model")
    sys.modules["model.warplayer"] = warp_module
    try:
        exec(compile(sources["IFNet_HDv3.py"], "<pinned-rife-ifnet-425>", "exec"), network_module.__dict__)
    finally:
        for name, previous in (("model", previous_root), ("model.warplayer", previous_warp)):
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous
    state = torch.load(io.BytesIO(sources["flownet.pkl"]), map_location="cpu", weights_only=True)
    if not isinstance(state, dict) or not all(isinstance(k, str) and isinstance(v, torch.Tensor) for k, v in state.items()):
        raise ValueError("Pinned weight file is not a tensor-only state dictionary")
    state = {key.removeprefix("module."): value for key, value in state.items()}
    network = network_module.IFNet()
    # The pinned IFNet explicitly comments out these two training-only modules;
    # the author's archived loader uses strict=False for that checkpoint. Keep
    # strict matching for every inference tensor and reject other unknown keys.
    state = inference_state(state, network.state_dict())
    network.load_state_dict(state, strict=True)
    network.eval()
    return network, network_module, warp_module


def export_pack(source_directory, destination, manifest, numerical_profile="strict"):
    if destination.exists():
        raise FileExistsError(f"Model pack already exists; choose a new versioned directory: {destination}")
    sources = verified_sources(source_directory, manifest)
    profiles_by_name = {"strict": {"maximum": 2e-4, "psnr": 80.0},
                        "sdr8": {"maximum": 0.5 / 255.0, "psnr": 80.0}}
    limits = profiles_by_name[numerical_profile]
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    import torch.nn.functional as functional

    torch.set_num_threads(2)
    torch.set_num_interop_threads(1)
    torch.manual_seed(425)
    network, source_module, warp_module = load_network(sources)

    class ExportWrapper(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.network = network

        def forward(self, frame0, frame1, timestep):
            # Matches the archived RIFE_HDv3.inference scale list for version 4.25.
            return self.network(torch.cat((frame0, frame1), 1), timestep, [16, 8, 4, 2, 1])[-1][-1]

    def dynamic_warp(pixels, flow):
        # Same grid_sample contract as upstream; no Python grid cache can freeze spatial shapes.
        batch, _, height, width = flow.shape
        horizontal = torch.linspace(-1.0, 1.0, width, device=flow.device, dtype=flow.dtype).view(1, 1, 1, width).expand(batch, 1, height, width)
        vertical = torch.linspace(-1.0, 1.0, height, device=flow.device, dtype=flow.dtype).view(1, 1, height, 1).expand(batch, 1, height, width)
        normalized_flow = torch.cat((flow[:, :1] / ((pixels.shape[3] - 1.0) / 2.0), flow[:, 1:2] / ((pixels.shape[2] - 1.0) / 2.0)), 1)
        grid = (torch.cat((horizontal, vertical), 1) + normalized_flow).permute(0, 2, 3, 1)
        return functional.grid_sample(pixels, grid, mode="bilinear", padding_mode="border", align_corners=True)

    model = ExportWrapper().eval()
    profiles = [(64, 64), (128, 192)]
    timesteps = [0.2, 0.5, 0.8]
    cases = []
    started = time.perf_counter()
    with torch.inference_mode():
        for height, width in profiles:
            warp_module.backwarp_tenGrid.clear()
            frame0 = torch.rand(1, 3, height, width)
            frame1 = torch.roll(frame0, shifts=(2, 3), dims=(2, 3))
            for timestep in timesteps:
                time_tensor = torch.full((1, 1, 1, 1), timestep)
                reference = model(frame0, frame1, time_tensor).numpy().copy()
                if not np.isfinite(reference).all() or reference.min() < -1e-6 or reference.max() > 1.000001:
                    raise ValueError("Upstream PyTorch reference produced invalid normalized RGB values")
                cases.append((height, width, timestep, frame0, frame1, time_tensor, reference))

        source_module.warp = dynamic_warp
        adapter_error = 0.0
        for height, width, timestep, frame0, frame1, time_tensor, reference in cases:
            adapted = model(frame0, frame1, time_tensor).numpy()
            adapter_error = max(adapter_error, float(np.max(np.abs(adapted - reference))))
        if adapter_error > 1e-6:
            raise ValueError(f"Export warp adapter differs from the upstream reference: {adapter_error}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    parent = destination.parent.resolve()
    temporary = Path(tempfile.mkdtemp(prefix=".rifelove-export-", dir=parent))
    try:
        onnx_path = temporary / "model.onnx"
        sample = cases[0]
        with torch.inference_mode():
            torch.onnx.export(model, (sample[3], sample[4], sample[5]), str(onnx_path),
                              input_names=["frame0", "frame1", "timestep"], output_names=["frame"],
                              opset_version=17, dynamo=False, do_constant_folding=True,
                              dynamic_axes={name: {2: "height", 3: "width"} for name in ("frame0", "frame1", "frame")})
        graph = onnx.load(onnx_path)
        onnx.helper.set_model_props(graph, {"model_id": manifest["modelId"], "license": "MIT",
                                          "upstream_commit": manifest["upstreamCommit"], "spatial_multiple": "64"})
        onnx.checker.check_model(graph)
        if {value.name for value in graph.graph.input} != {"frame0", "frame1", "timestep"}:
            raise ValueError("ONNX export froze or removed a required dynamic input")
        # The legacy exporter leaves the merged output channel symbolic even
        # though both warped RGB inputs have exactly three channels. Publish
        # the adapter's actual fixed batch/channel contract, retaining H/W.
        output_shape = graph.graph.output[0].type.tensor_type.shape.dim
        if len(output_shape) != 4:
            raise ValueError("Unexpected RIFE output tensor rank")
        output_shape[0].dim_value = 1
        output_shape[1].dim_value = 3
        onnx.checker.check_model(graph)
        onnx.save(graph, onnx_path)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 2
        options.inter_op_num_threads = 1
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_EXTENDED
        session = ort.InferenceSession(str(onnx_path), sess_options=options, providers=["CPUExecutionProvider"])
        reference_directory = temporary / "reference"
        reference_directory.mkdir()
        validation = []
        for height, width, timestep, frame0, frame1, time_tensor, expected in cases:
            values = {"frame0": frame0.numpy(), "frame1": frame1.numpy(), "timestep": time_tensor.numpy()}
            actual = session.run(["frame"], values)[0]
            if actual.shape != (1, 3, height, width) or not np.isfinite(actual).all():
                raise ValueError("ONNX output shape or numeric values are invalid")
            difference = np.abs(actual - expected)
            maximum = float(difference.max())
            mean_square = float(np.mean(difference.astype(np.float64) ** 2))
            psnr = float(-10 * np.log10(mean_square)) if mean_square else 999.0
            case_id = f"h{height}-w{width}-t{int(timestep * 100)}"
            fixtures = {}
            for name, array in {**values, "expected": expected}.items():
                data = np.ascontiguousarray(array, dtype="<f4").tobytes()
                filename = f"{case_id}-{name}.f32"
                (reference_directory / filename).write_bytes(data)
                fixtures[name] = {"file": f"reference/{filename}", "shape": list(array.shape), "sha256": digest(data)}
            validation.append({"id": case_id, "shape": [1, 3, height, width], "timestep": timestep,
                               "max_abs_error": maximum, "mean_abs_error": float(difference.mean()), "psnr_db": psnr,
                               "passed": maximum <= limits["maximum"] and psnr >= limits["psnr"], "fixtures": fixtures})
        graph_hash = digest(onnx_path.read_bytes())
        passed = all(case["passed"] for case in validation)
        report = {"status": "passed_cpu_reference" if passed else "failed_cpu_reference", "providers": session.get_providers(),
                  "torch": torch.__version__, "onnx": onnx.__version__, "onnxruntime": ort.__version__,
                  "model_sha256": graph_hash, "numerical_profile": numerical_profile,
                  "warp_adapter_max_abs_error": adapter_error, "ort_max_abs_error_limit": limits["maximum"], "minimum_psnr_db": limits["psnr"],
                  "cases": validation, "elapsed_seconds": time.perf_counter() - started,
                  "pending": ["native_onnx_cuda_parity", "tensorrt_fp32_fp16", "visual_quality", "performance_matrix"]}
        if not passed:
            failure = parent / (destination.name + ".failed-validation.json")
            failure.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            raise ValueError(f"ONNX/PyTorch numerical gate failed ({numerical_profile}); no pack published. Report: {failure}")
        pack_id = "rife-4.25-fp32" + ("-sdr8" if numerical_profile == "sdr8" else "")
        descriptor = {"schemaVersion": 1, "id": pack_id, "family": "rife", "version": "4.25",
                      "modelFile": "model.onnx", "sha256": graph_hash, "license": "MIT", "licenseFile": "LICENSE.txt",
                      "inputLayout": "NCHW", "dtype": "float32", "channels": 3, "batch": 1,
                      "inputRange": [0.0, 1.0], "spatialMultiple": 64,
                      "timestepShape": [1, 1, 1, 1], "dynamicTimestep": True, "dynamicSpatial": True,
                      "validatedBackends": ["pytorch_cpu", "onnxruntime_cpu"],
                      "numericalProfile": numerical_profile, "hdrQualified": False,
                      "validationFile": "validation.json", "sourceManifestFile": "source-manifest.json"}
        for filename, value in (("validation.json", report), ("model.json", descriptor), ("source-manifest.json", manifest)):
            (temporary / filename).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        (temporary / "LICENSE.txt").write_bytes(sources["LICENSE.txt"])
        if temporary.resolve().parent != parent or not temporary.name.startswith(".rifelove-export-"):
            raise ValueError("Unexpected export staging path")
        if destination.exists():
            raise FileExistsError("Model pack destination was created while export was running")
        os.rename(temporary, destination)
        return descriptor, report
    finally:
        if temporary.exists() and temporary.resolve().parent == parent and temporary.name.startswith(".rifelove-export-"):
            shutil.rmtree(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=ROOT / "models/weights/rife4.25-reviewed")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "models/cache/rife-4.25-fp32")
    parser.add_argument("--numerical-profile", choices=["strict", "sdr8"], default="strict",
                        help="strict keeps the original 2e-4 gate; sdr8 separately qualifies half of one normalized 8-bit code value plus PSNR >=80 dB, not HDR")
    args = parser.parse_args()
    manifest = json.loads(DEFAULT_MANIFEST.read_text(encoding="utf-8"))
    descriptor, report = export_pack(args.source_dir.resolve(), args.output_dir.resolve(), manifest, args.numerical_profile)
    print(json.dumps({"event": "rife_export_validated", "path": str(args.output_dir), "sha256": descriptor["sha256"],
                      "validation": report["status"], "cases": len(report["cases"]),
                      "max_abs_error": max(case["max_abs_error"] for case in report["cases"])}))


if __name__ == "__main__":
    main()

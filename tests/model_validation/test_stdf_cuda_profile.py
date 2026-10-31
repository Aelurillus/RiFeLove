"""Offline checks for truthful native-provider classification; never uses CUDA."""
import importlib.util
import json
from pathlib import Path

import pytest

pytest.importorskip("torch")
pytest.importorskip("numpy")

path = Path(__file__).resolve().parents[2] / "tools/validation/stdf_cuda_reference.py"
spec = importlib.util.spec_from_file_location(path.stem, path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def node(op, provider, dtype="float", shape=None):
    return {"cat": "Node", "dur": 5, "args": {"provider": provider, "op_name": op,
            "output_type_shape": [{dtype: [1, 1, 64, 64] if shape is None else shape}]}}


def summarize(tmp_path, events):
    profile = tmp_path / "profile.json"
    profile.write_text(json.dumps(events))
    return module.profile_summary(profile)


def test_explicit_cpu_shape_nodes_are_reported(tmp_path):
    result = summarize(tmp_path, [node("Conv", "CUDAExecutionProvider"),
                                 node("GridSample", "CUDAExecutionProvider"),
                                 node("Gather", "CPUExecutionProvider", "int64", []),
                                 node("Cast", "CPUExecutionProvider", "float", [])])
    assert result["critical_compute_is_cuda"]
    assert result["cpu_only_scalar_shape_outputs"]
    assert result["provider_kernel_events"]["CPUExecutionProvider"] == 2


@pytest.mark.parametrize("op", ["Conv", "GridSample"])
def test_cpu_restoration_kernels_fail_the_gate(tmp_path, op):
    result = summarize(tmp_path, [node("Conv", "CUDAExecutionProvider"),
                                 node("GridSample", "CUDAExecutionProvider"),
                                 node(op, "CPUExecutionProvider")])
    assert not result["critical_compute_is_cuda"]
    assert not result["cpu_only_scalar_shape_outputs"]


def test_cpu_float_image_math_cannot_masquerade_as_shape_work(tmp_path):
    result = summarize(tmp_path, [node("Conv", "CUDAExecutionProvider"),
                                 node("GridSample", "CUDAExecutionProvider"),
                                 node("Mul", "CPUExecutionProvider")])
    assert result["critical_compute_is_cuda"]
    assert not result["cpu_only_scalar_shape_outputs"]

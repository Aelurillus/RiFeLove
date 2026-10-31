"""CPU-only checks for the independent native adapter's Python packing oracle."""
import importlib.util
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("onnxruntime")
path = Path(__file__).resolve().parents[2] / "tools/validation/stdf_luma_adapter_cpu.py"
spec = importlib.util.spec_from_file_location(path.stem, path)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def test_independent_temporal_spatial_repetition(tmp_path):
    source = np.array([[16, 64, 235], [17, 65, 234]], dtype=np.uint8)
    source.tofile(tmp_path / "real-0-y.u8")
    item = {"source_size": [3, 2], "shape": [1, 7, 8, 8], "context_slots": [0] * 7}
    result = adapter.packing_oracle(item, tmp_path)
    assert result.shape == (1, 7, 8, 8)
    assert result[0, 0, 0, 0] == np.float32(16) / np.float32(255)
    assert result[0, 6, 7, 7] == np.float32(234) / np.float32(255)
    assert np.array_equal(result[0, 0], result[0, 6])


def test_independent_crop_clamp_encoded_blend():
    prediction = np.full((1, 1, 8, 8), 0.5, dtype=np.float32)
    prediction[0, 0, 0, :3] = [-1, 2, 0.5]
    actual = adapter.quantization_oracle(prediction, np.array([16, 100, 235], dtype=np.uint8), 3, 1, 0.5)
    assert actual.tolist() == [[8, 178, 181]]


@pytest.mark.parametrize("bad", [float("nan"), float("inf")])
def test_nonfinite_prediction_cannot_be_cast_to_silent_bytes(bad):
    with pytest.raises(ValueError):
        adapter.quantization_oracle(np.full((1, 1, 8, 8), bad, dtype=np.float32), np.array([16], dtype=np.uint8), 1, 1)


def test_wrong_graph_identity_is_rejected(tmp_path):
    graph = tmp_path / "wrong.onnx"
    graph.write_bytes(b"unverified model")
    with pytest.raises(ValueError, match="SHA-256"):
        adapter.verified_graph(graph)


def test_short_visible_plane_is_rejected(tmp_path):
    (tmp_path / "real-0-y.u8").write_bytes(b"x")
    with pytest.raises(ValueError, match="length"):
        adapter.packing_oracle({"source_size": [3, 2], "shape": [1, 7, 8, 8], "context_slots": [0] * 7}, tmp_path)


def test_invalid_spatial_padding_is_rejected(tmp_path):
    with pytest.raises(ValueError, match="geometry"):
        adapter.packing_oracle({"source_size": [3, 2], "shape": [1, 7, 9, 9], "context_slots": [0] * 7}, tmp_path)

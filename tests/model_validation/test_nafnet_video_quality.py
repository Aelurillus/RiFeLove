import hashlib
import json

import numpy as np
import pytest

from tools.validation.nafnet_video_quality import horizontal_box_blur, metrics, verified_graph


def test_horizontal_blur_preserves_constant_and_spreads_an_impulse():
    constant = np.full((2, 4, 9, 3), 123, dtype=np.uint8)
    assert np.array_equal(horizontal_box_blur(constant, 9), constant)
    impulse = np.zeros((2, 1, 9, 3), dtype=np.uint8)
    impulse[:, :, 4, :] = 90
    blurred = horizontal_box_blur(impulse, 3)
    assert blurred[0, 0, :, 0].tolist() == [0, 0, 0, 30, 30, 30, 0, 0, 0]
    assert np.array_equal(blurred[0], blurred[1])


def test_reference_metrics_measure_error_and_temporal_change():
    reference = np.full((3, 4, 5, 3), 100, dtype=np.uint8)
    exact = metrics(reference, reference)
    assert exact["psnr_db"] is None
    assert exact["rmse_levels"] == 0
    assert exact["temporal_error_difference_rmse_levels"] == 0
    shifted = reference.copy()
    shifted[1] += 10
    result = metrics(shifted, reference)
    assert result["rmse_levels"] > 0
    assert result["temporal_error_difference_rmse_levels"] > 0
    assert result["psnr_db"] is not None
    with pytest.raises(ValueError):
        metrics(reference[:2], reference)
    with pytest.raises(ValueError):
        horizontal_box_blur(reference, 4)


def test_reported_graph_must_match_descriptor(tmp_path):
    graph = tmp_path / "model.onnx"
    graph.write_bytes(b"graph")
    descriptor = tmp_path / "model.json"
    descriptor.write_text(json.dumps({"modelFile": graph.name, "sha256": hashlib.sha256(b"graph").hexdigest()}))
    assert verified_graph(tmp_path)[0] == graph
    graph.write_bytes(b"changed")
    with pytest.raises(ValueError, match="SHA-256"):
        verified_graph(tmp_path)
    descriptor.write_text(json.dumps({"modelFile": "other.onnx", "sha256": "unused"}))
    with pytest.raises(ValueError, match="modelFile"):
        verified_graph(tmp_path)

"""Contract tests for the recorded-luma STDF quality study."""
from __future__ import annotations

import json
from pathlib import Path
import sys

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/validation"))

import stdf_real_luma_quality as quality


class CenterSession:
    """Return the center member of every seven-frame tile window."""

    def get_providers(self):
        return ["CPUExecutionProvider"]

    def run(self, _outputs, feeds):
        window = feeds["luma_frames"]
        return [window[:, 3:4].copy()]


def test_predict_sequence_crops_tiles_and_preserves_every_visible_pixel():
    source = np.arange(9 * 264 * 520, dtype=np.uint32).reshape(9, 264, 520)
    source = (source % 220 + 16).astype(np.uint8)
    predicted = quality.predict_sequence(CenterSession(), source, [])
    np.testing.assert_array_equal(predicted, source.astype(np.float32) / 255.0)


def test_scene_windows_never_cross_explicit_cut():
    assert quality.scene_window(3, 12, [6]) == [0, 1, 2, 3, 4, 5, 5]
    assert quality.scene_window(6, 12, [6]) == [6, 6, 6, 6, 7, 8, 9]
    assert quality.scene_window(11, 12, [6]) == [8, 9, 10, 11, 11, 11, 11]


@pytest.mark.parametrize(
    "bad",
    [
        np.zeros((6, 32, 32), dtype=np.uint8),
        np.zeros((7, 31, 32), dtype=np.uint8),
        np.zeros((7, 32, 31), dtype=np.uint8),
        np.zeros((7, 32, 32), dtype=np.float32),
        np.zeros((7, 32), dtype=np.uint8),
        np.zeros((121, 32, 32), dtype=np.uint8),
        np.zeros((7, 32, 3842), dtype=np.uint8),
    ],
)
def test_validation_rejects_bad_sequence_contracts(bad):
    with pytest.raises(ValueError):
        quality.validate_sequence(bad)


def test_strength_zero_is_identity_and_nonfinite_prediction_fails():
    source = np.full((7, 32, 32), 96, dtype=np.uint8)
    prediction = np.full(source.shape, 0.8, dtype=np.float32)
    np.testing.assert_array_equal(quality.blend_sequence(prediction, source, 0.0), source)
    with pytest.raises(ValueError):
        quality.blend_sequence(np.full(source.shape, np.nan, dtype=np.float32), source, 1.0)
    with pytest.raises(ValueError):
        quality.blend_sequence(prediction, source, 1.1)


def test_comparison_reports_improvement_and_regression_per_frame():
    reference = np.full((7, 32, 32), 128, dtype=np.uint8)
    source = reference.copy()
    source[:, 8:24, 8:24] = 160
    restored = source.copy()
    restored[:, 8:24, 8:24] = 140
    report = quality.comparison(restored, reference, source)
    assert report["frames_improved_mse"] == 7
    assert report["frames_worsened_mse"] == 0
    assert report["pixels_changed"] == 7 * 16 * 16
    assert report["delta_psnr_y_db"] > 0
    harm = quality.comparison(source, reference, reference)
    assert harm["frames_worsened_mse"] == 7
    assert harm["delta_psnr_y_db"] is None  # exact baseline has infinite PSNR
    assert harm["input"]["rmse_y_levels"] == 0
    assert harm["output"]["rmse_y_levels"] > 0
    with pytest.raises(ValueError, match="uint8"):
        quality.comparison(source.astype(float), reference, source)


def test_native_fixture_manifest_is_published_last_with_complete_hashes(tmp_path, monkeypatch):
    graph = tmp_path / "model.onnx"
    graph.write_bytes(b"test-graph")
    monkeypatch.setattr(quality, "GRAPH_SHA256", quality.digest(graph))
    source = np.full((7, 32, 32), 96, dtype=np.uint8)
    prediction = source.astype(np.float32) / 255.0
    output = tmp_path / "fixture"
    manifest = quality.write_native_fixture(output, source, prediction, 3, [], graph)
    on_disk = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    assert on_disk == manifest
    assert on_disk["allOutputPixels"] is True
    assert on_disk["wholeFrameOracle"] is False
    for name, identity in on_disk["files"].items():
        path = output / name
        assert path.stat().st_size == identity["bytes"]
        assert quality.digest(path) == identity["sha256"]
    assert (output / "input-y7.u8").stat().st_size == 7 * 32 * 32
    np.testing.assert_array_equal(np.fromfile(output / "reference-y.u8", np.uint8).reshape(32, 32), source[3])
    with pytest.raises(FileExistsError):
        quality.write_native_fixture(output, source, prediction, 3, [], graph)


def test_native_fixture_failure_never_publishes_manifest(tmp_path, monkeypatch):
    graph = tmp_path / "model.onnx"
    graph.write_bytes(b"test-graph")
    source = np.full((7, 32, 32), 96, dtype=np.uint8)
    output = tmp_path / "fixture"
    with pytest.raises(ValueError, match="pinned"):
        quality.write_native_fixture(output, source, source.astype(np.float32), 3, [], graph)
    assert not output.exists()
    monkeypatch.setattr(quality, "GRAPH_SHA256", quality.digest(graph))
    with pytest.raises(ValueError, match="prediction"):
        quality.write_native_fixture(output, source, np.full(source.shape, np.nan), 3, [], graph)
    assert not (output / "manifest.json").exists()


@pytest.mark.parametrize("mode", ("shape", "nan"))
def test_bad_inference_result_rejected(mode):
    class BadSession:
        def run(self, _outputs, feeds):
            value = feeds["luma_frames"][:, 3:4].copy()
            if mode == "nan":
                value[0, 0, -1, -1] = np.nan
            else:
                value = value[:, :, :-1, :]
            return [value]
    with pytest.raises(ValueError, match="incomplete or non-finite"):
        quality.predict_sequence(BadSession(), np.full((7, 34, 34), 96, dtype=np.uint8), [])

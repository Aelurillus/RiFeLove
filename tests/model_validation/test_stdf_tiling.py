"""Geometry and consistency checks for the standalone STDF tiling oracle."""
from pathlib import Path
import importlib.util
import weakref

import numpy as np
import pytest


path = Path(__file__).resolve().parents[2] / "tools/validation/stdf_tiling_compare.py"
spec = importlib.util.spec_from_file_location(path.stem, path)
tiling = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tiling)

bounded_path = Path(__file__).resolve().parents[2] / "tools/validation/stdf_tiling_bounded.py"
bounded_spec = importlib.util.spec_from_file_location(bounded_path.stem, bounded_path)
bounded = importlib.util.module_from_spec(bounded_spec)
bounded_spec.loader.exec_module(bounded)


class ChannelZeroSession:
    """Independent local operator used to test tile coverage and crop layout."""

    def run(self, _outputs, feeds):
        source = feeds["luma_frames"]
        return [source[:, :1].copy()]


def test_outer_extent_is_clamped_without_synthetic_bias_pixels():
    frames = np.arange(1 * 7 * 16 * 24, dtype=np.float32).reshape(1, 7, 16, 24)
    tile, crop = tiling._pad_tile(frames, 0, 0, 8, 8, 16)
    assert tile.shape == (1, 7, 16, 24)
    assert crop == (0, 0, 8, 8)
    np.testing.assert_array_equal(tile, frames)


def test_tiled_operator_covers_each_pixel_once():
    frames = np.arange(1 * 7 * 32 * 48, dtype=np.float32).reshape(1, 7, 32, 48)
    output, count = tiling.tiled_infer(ChannelZeroSession(), frames, 16, 16, 8)
    assert count == 6
    np.testing.assert_array_equal(output, frames[:, :1])


def test_invalid_tile_geometry_fails_closed():
    frames = np.zeros((1, 7, 32, 48), dtype=np.float32)
    with pytest.raises(ValueError, match="multiples of 8"):
        tiling.tiled_infer(ChannelZeroSession(), frames, 10, 16, 8)
    with pytest.raises(ValueError, match="multiples of 8"):
        tiling.tiled_infer(ChannelZeroSession(), frames, 16, 16, 3)


def test_metrics_expose_seam_error_separately():
    reference = np.zeros((1, 1, 16, 16), dtype=np.float32)
    actual = reference.copy()
    actual[:, :, :, 8] = 1.0
    result = tiling.metrics(reference, actual, 8, 8)
    assert result["maximum_absolute_error"] == 1.0
    assert result["seam_mean_absolute_error"] > result["nonseam_mean_absolute_error"]


def test_qualified_graph_has_zero_error_when_each_tile_is_the_whole_frame():
    """Keep one small end-to-end check when the reviewed local graph is present."""
    graph = Path("work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx")
    if not graph.exists():
        pytest.skip("local STDF feasibility graph is not available")
    import onnxruntime as ort

    session = ort.InferenceSession(str(graph), providers=["CPUExecutionProvider"])
    frames = tiling.make_cases(64, 64)[2][1]
    reference = session.run(None, {"luma_frames": frames})[0]
    actual, count = tiling.tiled_infer(session, frames, 32, 32, 32)
    assert count == 4
    result = tiling.metrics(reference, actual, 32, 32)
    assert result["maximum_absolute_error"] <= 3e-5
    assert result["maximum_8bit_difference"] <= 1


def test_bounded_case_tile_is_coordinate_stable():
    full = bounded.case_tile("large_displacement_edge", 64, 96, 0, 0, 96, 64)
    part = bounded.case_tile("large_displacement_edge", 64, 96, 32, 16, 96, 64)
    np.testing.assert_array_equal(part, full[:, :, 16:64, 32:96])


def test_bounded_tile_grid_has_no_gaps_or_overlap():
    tiles = list(bounded.iter_tiles(64, 96, 32, 32))
    coverage = np.zeros((64, 96), dtype=np.int16)
    for x0, y0, x1, y1 in tiles:
        coverage[y0:y1, x0:x1] += 1
    assert len(tiles) == 6
    assert np.all(coverage == 1)


def test_bounded_runner_can_stream_without_full_output_buffer():
    report = bounded.run_shape(ChannelZeroSession(), 32, 48, 16, 16, [8, 16],
                               ["moving_textured_square"], reference=True, sample_limit=None)
    case = report["cases"][0]
    assert case["policies"][0]["vs_whole_frame"]["maximum_absolute_error"] == 0.0
    assert case["policies"][1]["vs_whole_frame"]["maximum_absolute_error"] == 0.0
    assert case["policies"][0]["peak_host_tensor_bytes"] > 0


def test_bounded_runner_releases_previous_tile_outputs():
    class TrackedSession(ChannelZeroSession):
        def __init__(self):
            self.outputs = []
            self.maximum_live = 0

        def run(self, outputs, feeds):
            self.maximum_live = max(self.maximum_live, sum(ref() is not None for ref in self.outputs))
            result = super().run(outputs, feeds)
            self.outputs.append(weakref.ref(result[0]))
            return result

    session = TrackedSession()
    report = bounded.run_shape(session, 64, 96, 16, 16, [8, 16, 24],
                               ["moving_textured_square"], reference=False, sample_limit=None)
    assert report["tiles_total"] == 24
    assert session.maximum_live <= 1
    assert report["cases"][0]["pair_consistency"][0]["maximum_absolute_error"] == 0.0
    assert report["cases"][0]["policies"][0]["peak_tracked_host_tensor_bytes"] > 0


def test_large_grid_count_and_invalid_halos():
    assert len(list(bounded.iter_tiles(1080, 1920, 384, 256))) == 25
    assert len(list(bounded.iter_tiles(2160, 3840, 384, 256))) == 90
    for halos in ([], [-8], [8, 8], [3]):
        with pytest.raises(ValueError, match="halos"):
            bounded.run_shape(ChannelZeroSession(), 32, 48, 16, 16, halos,
                              ["moving_textured_square"], reference=False, sample_limit=None)

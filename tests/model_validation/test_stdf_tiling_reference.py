"""CPU-only independent geometry, strided-plane and raw-output contract checks."""
from fractions import Fraction
import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/validation"))
import stdf_tiling_reference as reference


class CenterSession:
    def get_providers(self):
        return ["CPUExecutionProvider"]

    def run(self, _outputs, feeds):
        return [feeds["luma_frames"][:, 3:4].copy()]


@pytest.mark.parametrize("width,height,count", [(3840, 2160, 255), (2160, 3840, 270),
                                                 (1920, 1080, 72), (854, 480, 16),
                                                 (1918, 1078, 72), (519, 263, 9), (1, 1, 1)])
def test_full_geometry_covers_every_output_pixel_exactly_once(width, height, count):
    tiles = reference.tile_plan(width, height)
    coverage = np.zeros((height, width), dtype=np.uint8)
    assert len(tiles) == count
    for tile in tiles:
        x, y, right, bottom = tile.core
        sx, sy, sr, sb = tile.source
        coverage[y:bottom, x:right] += 1
        assert 0 <= sx <= x < right <= sr <= width
        assert 0 <= sy <= y < bottom <= sb <= height
        assert sx % 8 == sy % 8 == 0
        assert tile.padded[0] % 8 == tile.padded[1] % 8 == 0
        assert tile.padded[0] <= 384 and tile.padded[1] <= 256
        if sr < width:
            assert tile.padded[0] == sr - sx  # Never pad at an internal edge.
        if sb < height:
            assert tile.padded[1] == sb - sy
    assert np.all(coverage == 1)


def test_tail_source_is_real_clamped_then_only_outer_edge_is_padded():
    tile = reference.tile_plan(519, 263)[-1]
    assert tile.core == (512, 256, 519, 263)
    assert tile.source == (448, 192, 519, 263)
    assert tile.padded == (72, 72)
    assert tile.crop == (64, 64, 71, 71)
    source = reference.tensor("coordinate_texture", 519, 263, tile.source)
    assert source.shape == (1, 7, 72, 72)
    np.testing.assert_array_equal(source[..., -1, :], source[..., -2, :])
    np.testing.assert_array_equal(source[..., :, -1], source[..., :, -2])


@pytest.mark.parametrize("width,height,halo", [(0, 1, 64), (-8, 1, 64), (3841, 8, 64),
                                              (3840, 2161, 64), (3840, 3840, 64),
                                              (64, 64, -8), (64, 64, 65), (True, 64, 64)])
def test_unsafe_geometry_fails_closed(width, height, halo):
    with pytest.raises(ValueError):
        reference.tile_plan(width, height, halo)


@pytest.mark.parametrize("case", reference.CASES)
def test_tile_generator_is_global_coordinate_stable_for_motion_texture_and_edges(case):
    full = reference.tensor(case, 519, 263)
    tile = reference.tile_plan(519, 263)[4]
    sx, sy, sr, sb = tile.source
    part = reference.tensor(case, 519, 263, tile.source)
    np.testing.assert_array_equal(part[..., :sb-sy, :sr-sx], full[..., sy:sb, sx:sr])
    np.testing.assert_array_equal(part, full[..., sy:sy + tile.padded[1], sx:sx + tile.padded[0]])
    reconstructed = reference.tiled_prediction(CenterSession(), case, 519, 263)
    np.testing.assert_array_equal(reconstructed, full[0, 3, :263, :519])


def test_fixture_has_seven_strided_planes_sentinels_odd_chroma_and_exact_vfr_pts(tmp_path):
    directory = tmp_path / "fixture"
    manifest = reference.write_fixture(directory, 67, 49, "coordinate_texture")
    loaded, packed = reference.read_fixture(directory / "fixture.json")
    np.testing.assert_array_equal(packed, reference.tensor("coordinate_texture", 67, 49))
    assert loaded == manifest
    assert packed.shape == (1, 7, 56, 72)
    for slot, frame in enumerate(manifest["frames"]):
        assert Fraction(*frame["pts"]) == reference.pts(100 + slot)
        assert Fraction(*frame["duration"]) == reference.pts(101 + slot) - reference.pts(100 + slot)
        for plane in frame["planes"]:
            raw = np.fromfile(directory / plane["file"], dtype=np.uint8).reshape(plane["height"], plane["stride"])
            assert np.all(raw[:, plane["width"]:] == 250)
        assert frame["planes"][1]["width"] == 34
        assert frame["planes"][1]["height"] == 25
    assert len({Fraction(*f["duration"]) for f in manifest["frames"]}) > 1
    with pytest.raises(FileExistsError):
        reference.write_fixture(directory, 67, 49, "coordinate_texture")


def test_fixture_reader_rejects_changed_bytes_and_untrusted_plane_paths(tmp_path):
    directory = tmp_path / "fixture"
    manifest = reference.write_fixture(directory, 9, 7, "hard_edge")
    plane = directory / manifest["frames"][0]["planes"][0]["file"]
    raw = bytearray(plane.read_bytes())
    raw[0] ^= 1
    plane.write_bytes(raw)
    with pytest.raises(ValueError, match="SHA-256"):
        reference.read_fixture(directory / "fixture.json")
    manifest["frames"][0]["planes"][0]["file"] = "../outside.u8"
    (directory / "fixture.json").write_text(json.dumps(manifest), encoding="utf-8")
    with pytest.raises(ValueError, match="escapes"):
        reference.read_fixture(directory / "fixture.json")


def test_lround_half_up_is_not_numpy_bankers_rounding():
    prediction = np.array([[0.5, -0.1, 1.1]], dtype=np.float32)
    original = np.array([[16, 20, 30]], dtype=np.uint8)
    np.testing.assert_array_equal(reference.quantize(prediction, original), [[128, 0, 255]])
    np.testing.assert_array_equal(reference.quantize(prediction, original, 0), original)
    with pytest.raises(ValueError):
        reference.quantize(np.array([[float("nan")]], dtype=np.float32), original[:, :1])


def test_native_comparison_checks_all_pixels_chroma_and_exact_metadata(tmp_path):
    directory = tmp_path / "fixture"
    manifest = reference.write_fixture(directory, 67, 49, "moving_texture")
    actual = tmp_path / "actual"
    actual.mkdir()
    center = manifest["frames"][3]
    metadata = {"width": 67, "height": 49, "center_index": center["index"],
                "center_pts": center["pts"], "center_duration": center["duration"],
                "timestamp_recovered": center["timestampRecovered"], "color": reference.COLOR}
    (actual / "output.json").write_text(json.dumps(metadata), encoding="utf-8")
    for plane, name in enumerate(("y", "u", "v")):
        (actual / f"output-{name}.u8").write_bytes((directory / f"center-{plane}.u8").read_bytes())
    report = reference.compare_native(directory / "fixture.json", actual)
    assert report["passed"]
    assert report["planes"][0]["compared_pixels"] == 67 * 49
    # A single last-pixel error must fail; no sparse output sampling is accepted.
    y = bytearray((actual / "output-y.u8").read_bytes())
    y[-1] ^= 1
    (actual / "output-y.u8").write_bytes(y)
    assert not reference.compare_native(directory / "fixture.json", actual)["passed"]
    metadata["center_pts"][0] += 1
    (actual / "output.json").write_text(json.dumps(metadata), encoding="utf-8")
    with pytest.raises(ValueError, match="metadata"):
        reference.compare_native(directory / "fixture.json", actual)


def test_halo96_is_research_comparator_outside_production_tile_envelope():
    assert max(t.padded[0] for t in reference.tile_plan(3840, 2160, 96)) == 448
    assert max(t.padded[1] for t in reference.tile_plan(3840, 2160, 96)) == 320


def test_raw_fixture_and_full_oracle_fail_closed_for_large_shapes(tmp_path):
    with pytest.raises(ValueError, match="bounded"):
        reference.write_fixture(tmp_path / "too-large", 3840, 2160, "hard_edge")
    with pytest.raises(ValueError, match="small shapes"):
        reference.small_oracle(Path("missing.onnx"), tmp_path / "oracle", 3840, 2160, "hard_edge")


@pytest.mark.parametrize("oracle", ("whole", "tiled"))
def test_native_raw_protocol_generates_every_visible_byte_with_explicit_oracle(tmp_path, monkeypatch, oracle):
    """Fake center inference isolates file/geometry contracts, not model accuracy."""
    monkeypatch.setattr(reference, "cpu_session", lambda _graph, _threads: CenterSession())
    graph = tmp_path / "graph.onnx"
    graph.write_bytes(b"test-session-placeholder")
    output = tmp_path / "native-fixture"
    report = reference.write_native_fixture(graph, output, 519, 263, "moving_texture", oracle)
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    assert report["passed"] and report["tiles_completed"] == 9
    assert report["all_output_pixels"] and report["gpu_used"] is False
    assert manifest["wholeFrameOracle"] is (oracle == "whole")
    assert manifest["oracle"] == ("cpu-whole-frame-global-pad8" if oracle == "whole" else
                                   "cpu-independent-tiled-global-pad8")
    expected = np.stack([reference.visible_plane("moving_texture", 519, 263, i) for i in range(100, 107)])
    np.testing.assert_array_equal(np.fromfile(output / "input-y7.u8", np.uint8).reshape(7, 263, 519), expected)
    np.testing.assert_array_equal(np.fromfile(output / "reference-y.u8", np.uint8).reshape(263, 519), expected[3])
    for filename, identity in manifest["files"].items():
        assert reference.sha256(output / filename) == identity["sha256"]
        assert (output / filename).stat().st_size == identity["bytes"]


def test_native_raw_comparison_rejects_last_pixel_error_and_dishonest_summary(tmp_path, monkeypatch):
    monkeypatch.setattr(reference, "cpu_session", lambda _graph, _threads: CenterSession())
    graph = tmp_path / "graph.onnx"
    graph.write_bytes(b"test-session-placeholder")
    fixture = tmp_path / "fixture"
    reference.write_native_fixture(graph, fixture, 67, 49, "hard_edge", "tiled")
    actual = tmp_path / "actual"
    actual.mkdir()
    output = bytearray((fixture / "reference-y.u8").read_bytes())
    (actual / "native-y.u8").write_bytes(output)
    report = {"schemaVersion": 1, "graph_sha256": reference.GRAPH_SHA256,
              "tile_policy": reference.TILE_POLICY, "source_width": 67, "source_height": 49,
              "oracle": "cpu-independent-tiled-global-pad8", "strength": 1.0,
              "tiles_planned": 1, "tiles_completed": 1, "critical_compute_verified": True,
              "cpu_image_fallback": False, "whole_frame_oracle_present": False,
              "max_8bit_difference": 0, "changed_samples": 0, "passed": True,
              "mean_8bit_absolute_error": 0.0}
    (actual / "report.json").write_text(json.dumps(report), encoding="utf-8")
    assert reference.compare_native_luma(fixture, actual)["passed"]
    output[-1] -= 2
    (actual / "native-y.u8").write_bytes(output)
    with pytest.raises(ValueError, match="disagrees"):
        reference.compare_native_luma(fixture, actual)
    report.update({"max_8bit_difference": 2, "changed_samples": 1, "passed": False,
                   "mean_8bit_absolute_error": 2 / (67 * 49)})
    (actual / "report.json").write_text(json.dumps(report), encoding="utf-8")
    result = reference.compare_native_luma(fixture, actual)
    assert not result["passed"] and result["compared_pixels"] == 67 * 49
    assert result["metadata_roundtrip_in_this_protocol"] is False


def test_native_fixture_failure_retains_progress_but_does_not_publish_manifest(tmp_path, monkeypatch):
    class FailingSession(CenterSession):
        def run(self, _outputs, _feeds):
            raise RuntimeError("deliberate CPU inference failure")
    monkeypatch.setattr(reference, "cpu_session", lambda _graph, _threads: FailingSession())
    graph = tmp_path / "graph.onnx"
    graph.write_bytes(b"test-session-placeholder")
    fixture = tmp_path / "fixture"
    with pytest.raises(RuntimeError, match="deliberate"):
        reference.write_native_fixture(graph, fixture, 67, 49, "hard_edge", "tiled")
    assert not (fixture / "manifest.json").exists()
    report = json.loads((fixture / "cpu-reference-report.json").read_text(encoding="utf-8"))
    assert report["status"] == "failed" and not report["passed"]
    assert (fixture / "input-y7.u8").stat().st_size == 7 * 67 * 49


def test_cpp_policy_all_tiles_match_independent_geometry(tmp_path):
    """Compile only the pure policy header; no app, CMake, ORT, or GPU build."""
    root = Path(__file__).resolve().parents[2]
    header = root / "src/restoration/stdf_tile_policy.hpp"
    compiler = shutil.which("g++")
    if not header.exists() or not compiler:
        pytest.skip("optional native pure-policy compiler/header is unavailable")
    source = tmp_path / "geometry.cpp"
    source.write_text('''#include "restoration/stdf_tile_policy.hpp"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    namespace p = rifelove::restoration::stdf_tiles;
    const int w = std::stoi(argv[1]), h = std::stoi(argv[2]);
    if (!p::supportsDimensions(w,h)) return 3;
    for (std::size_t i=0; i<p::tileCount(w,h); ++i) {
        const auto t=*p::tileAt(w,h,i);
        std::cout << t.coreX << ' ' << t.coreY << ' ' << t.coreWidth << ' ' << t.coreHeight << ' '
                  << t.inputX << ' ' << t.inputY << ' ' << t.inputWidth << ' ' << t.inputHeight << '\\n';
    }
}
''', encoding="utf-8")
    executable = tmp_path / ("geometry.exe" if sys.platform == "win32" else "geometry")
    subprocess.run([compiler, "-std=c++20", "-static-libstdc++", "-static-libgcc",
                    "-I", str(root / "src"), str(source), "-o", str(executable)],
                   capture_output=True, check=True, timeout=60)
    for width, height in [(3840, 2160), (2160, 3840), (1920, 1080), (854, 480), (1918, 1078), (519, 263),
                           (391, 199), (67, 49), (1, 1)]:
        completed = subprocess.run([str(executable), str(width), str(height)],
                                   capture_output=True, text=True, check=True, timeout=10)
        native = [tuple(map(int, line.split())) for line in completed.stdout.splitlines()]
        expected = []
        for tile in reference.tile_plan(width, height):
            x, y, right, bottom = tile.core
            sx, sy, _, _ = tile.source
            expected.append((x, y, right-x, bottom-y, sx, sy, *tile.padded))
        assert native == expected
    for width, height in [(3840, 2161), (3841, 8), (0, 1)]:
        assert subprocess.run([str(executable), str(width), str(height)],
                              capture_output=True, timeout=10).returncode == 3

"""Independent, CPU-only STDF tiled-adapter fixtures and geometry oracle.

The fixture protocol is deliberately raw: seven strided YUV420p planes, exact
rational source PTS, and visible byte outputs. It never downloads models, uses
CUDA, edits product settings, or treats sampled large tiles as a full-frame
qualification. Existing evidence directories are never overwritten.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import time

import numpy as np

GRAPH_SHA256 = "dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721"
PROTOCOL = "rifelove-stdf-tiled-fixture-v1"
TILE_POLICY = "stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1"
MAX_PIXELS = 8_294_400
CASES = ("coordinate_texture", "moving_texture", "hard_edge", "checkerboard")
COLOR = {"pixelFormat": "yuv420p", "bitDepth": 8, "primaries": "bt709",
         "transfer": "bt709", "matrix": "bt709", "range": "tv", "hdrTransfer": False}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def ceil8(value: int) -> int:
    return (value + 7) // 8 * 8


@dataclass(frozen=True)
class Tile:
    core: tuple[int, int, int, int]
    source: tuple[int, int, int, int]
    padded: tuple[int, int]  # width, height; only true right/bottom edge repeats
    crop: tuple[int, int, int, int]


def tile_plan(width: int, height: int, halo: int = 64) -> list[Tile]:
    """Independent 256x128 core grid, including non-eight-aligned tail cores.

    Halo 96 is a CPU research comparator. Its input can exceed the production
    384x256 tile ceiling; it must never be advertised as a qualified GPU policy.
    """
    if any(isinstance(v, bool) or not isinstance(v, int) for v in (width, height, halo)):
        raise ValueError("integer frame dimensions and halo required")
    if min(width, height) <= 0 or max(width, height) > 3840 or width * height > MAX_PIXELS:
        raise ValueError("frame exceeds the UHD portrait/landscape pixel envelope")
    if halo < 0 or halo % 8:
        raise ValueError("halo must be a nonnegative multiple of eight")
    result = []
    for y in range(0, height, 128):
        for x in range(0, width, 256):
            right, bottom = min(x + 256, width), min(y + 128, height)
            sx, sy = max(0, x - halo), max(0, y - halo)
            sr, sb = min(width, right + halo), min(height, bottom + halo)
            result.append(Tile((x, y, right, bottom), (sx, sy, sr, sb),
                               (ceil8(sr - sx), ceil8(sb - sy)),
                               (x - sx, y - sy, right - sx, bottom - sy)))
    return result


def visible_plane(case: str, width: int, height: int, index: int, plane: int = 0,
                  extent: tuple[int, int, int, int] | None = None) -> np.ndarray:
    """Integer-only global-coordinate generator; independent of tile shape."""
    if case not in CASES or plane not in (0, 1, 2):
        raise ValueError("unknown fixture case/plane")
    w, h = (width, height) if plane == 0 else ((width + 1) // 2, (height + 1) // 2)
    x0, y0, x1, y1 = extent or (0, 0, w, h)
    if not (0 <= x0 < x1 <= w and 0 <= y0 < y1 <= h):
        raise ValueError("source extent is outside the real plane")
    yy, xx = np.indices((y1 - y0, x1 - x0), dtype=np.int64)
    xx += x0
    yy += y0
    if plane:
        image = 16 + (xx * 3 + yy * 5 + index * 17 + plane * 53) % 225
    elif case == "coordinate_texture":
        image = 16 + (xx * 7 + yy * 11 + index * 13 + ((xx + yy) % 5) * 9) % 220
    elif case == "checkerboard":
        image = 16 + (((xx + (index - 103) * 2) // 3 +
                       (yy + (index - 103) * 2) // 3) & 1) * 219
    else:
        image = 16 + (xx * 3 + yy * 5 + index * 17) % 48
        shift = (index - 103) * (13 if case == "moving_texture" else 29)
        left, top = max(1, width // 4 + shift), max(1, height // 3)
        size = max(8, min(width, height) // 3)
        inside = (xx >= left) & (xx < left + size) & (yy >= top) & (yy < top + size)
        if case == "moving_texture":
            image[inside] = 16 + (xx[inside] * 7 + yy[inside] * 11 + index * 13) % 220
        else:
            image[inside] = 235
    # Distinct true boundaries expose accidental synthetic halos and stride leaks.
    if plane == 0:
        image[(xx == 0) | (yy == 0)] = 16 + index % 7
        image[(xx == w - 1) | (yy == h - 1)] = 235 - index % 7
    return image.astype(np.uint8)


def tensor(case: str, width: int, height: int,
           extent: tuple[int, int, int, int] | None = None) -> np.ndarray:
    extent = extent or (0, 0, width, height)
    frames = np.stack([visible_plane(case, width, height, 100 + i, extent=extent)
                       for i in range(7)])
    h, w = frames.shape[-2:]
    padded = np.pad(frames, ((0, 0), (0, ceil8(h) - h), (0, ceil8(w) - w)), mode="edge")
    return np.ascontiguousarray(padded[None].astype(np.float32) / np.float32(255.0))


def pts(index: int) -> Fraction:
    return Fraction(index * 3003 + index % 3 * 77, 90_000)


def _rational(value: Fraction) -> list[int]:
    return [value.numerator, value.denominator]


def _fresh(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=False)


def write_fixture(output: Path, width: int, height: int, case: str) -> dict:
    tiles = tile_plan(width, height)
    if width * height > 393_216:
        raise ValueError("raw fixture writer is deliberately bounded; use coordinate tiles for large studies")
    _fresh(output)
    report = {"schemaVersion": 1, "protocol": PROTOCOL, "width": width, "height": height,
              "case": case, "centerSlot": 3, "byteOrder": "little", "color": COLOR,
              "paddingByte": 250, "frames": [], "core": [256, 128], "halo": 64,
              "tiles": [{"core": list(t.core), "source": list(t.source), "padded": list(t.padded),
                         "crop": list(t.crop)} for t in tiles]}
    for slot in range(7):
        index = 100 + slot
        frame = {"index": index, "pts": _rational(pts(index)),
                 "duration": _rational(pts(index + 1) - pts(index)),
                 "timestampRecovered": bool(slot % 2), "planes": []}
        for plane, padding in enumerate((5, 3, 7)):
            visible = visible_plane(case, width, height, index, plane)
            h, w = visible.shape
            stride = w + padding
            storage = np.full((h, stride), 250, dtype=np.uint8)
            storage[:, :w] = visible
            filename = f"frame-{slot}-plane-{plane}.u8"
            storage.tofile(output / filename)
            frame["planes"].append({"file": filename, "width": w, "height": h,
                                    "stride": stride, "bytes": storage.nbytes,
                                    "sha256": sha256(output / filename)})
            if slot == 3:
                visible.tofile(output / f"center-{plane}.u8")
        report["frames"].append(frame)
    (output / "fixture.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def read_fixture(path: Path) -> tuple[dict, np.ndarray]:
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if report["protocol"] != PROTOCOL or report["color"] != COLOR or report["centerSlot"] != 3:
        raise ValueError("fixture contract differs")
    tile_plan(report["width"], report["height"])
    if len(report["frames"]) != 7:
        raise ValueError("exactly seven real source frames required")
    frames = []
    for slot, frame in enumerate(report["frames"]):
        if (frame["index"] != 100 + slot or Fraction(*frame["pts"]) != pts(100 + slot)
                or Fraction(*frame["duration"]) != pts(101 + slot) - pts(100 + slot)
                or frame["timestampRecovered"] is not bool(slot % 2)):
            raise ValueError("exact rational source metadata differs")
        if len(frame["planes"]) != 3:
            raise ValueError("exactly three YUV420p planes required")
        for plane, layout in enumerate(frame["planes"]):
            w, h = ((report["width"], report["height"]) if plane == 0 else
                    ((report["width"] + 1) // 2, (report["height"] + 1) // 2))
            if layout["width"] != w or layout["height"] != h or layout["stride"] < w:
                raise ValueError("invalid plane geometry/stride")
            file = (path.parent / layout["file"]).resolve()
            if not file.is_relative_to(path.parent.resolve()) or file.is_symlink():
                raise ValueError("fixture plane escapes its evidence directory")
            if file.stat().st_size != h * layout["stride"] or sha256(file) != layout["sha256"]:
                raise ValueError("plane byte count or SHA-256 differs")
            storage = np.fromfile(file, dtype=np.uint8).reshape(h, layout["stride"])
            if plane == 0:
                frames.append(storage[:, :w].copy())
    array = np.stack(frames)
    h, w = array.shape[-2:]
    padded = np.pad(array, ((0, 0), (0, ceil8(h) - h), (0, ceil8(w) - w)), mode="edge")
    return report, np.ascontiguousarray(padded[None].astype(np.float32) / np.float32(255))


def quantize(prediction: np.ndarray, original: np.ndarray, strength: float = 1.0) -> np.ndarray:
    if not np.all(np.isfinite(prediction)) or not 0 <= strength <= 1:
        raise ValueError("invalid prediction or strength")
    center = original.astype(np.float32)
    restored = center + np.float32(strength) * (np.clip(prediction, 0, 1) * np.float32(255) - center)
    return np.floor(np.clip(restored, 0, 255) + np.float32(0.5)).astype(np.uint8)


def statistics(reference: np.ndarray, actual: np.ndarray, original: np.ndarray) -> dict:
    if reference.shape != actual.shape or not np.all(np.isfinite(actual)):
        raise ValueError("prediction shape/nonfinite mismatch")
    delta = np.abs(reference.astype(np.float64) - actual.astype(np.float64))
    byte_delta = np.abs(quantize(reference, original).astype(np.int16) -
                        quantize(actual, original).astype(np.int16))
    return {"compared_pixels": int(delta.size), "maximum_absolute_error": float(delta.max()),
            "mean_absolute_error": float(delta.mean()),
            "maximum_8bit_difference": int(byte_delta.max()),
            "changed_8bit_pixels": int(np.count_nonzero(byte_delta))}


def tiled_prediction(session, case: str, width: int, height: int, halo: int = 64) -> np.ndarray:
    """All cores written exactly once; test with independent local operators too."""
    result = np.empty((height, width), dtype=np.float32)
    for tile in tile_plan(width, height, halo):
        prediction = session.run(None, {"luma_frames": tensor(case, width, height, tile.source)})[0]
        if prediction.shape != (1, 1, tile.padded[1], tile.padded[0]):
            raise ValueError("inference output shape mismatch")
        x, y, right, bottom = tile.core
        cx, cy, cr, cb = tile.crop
        result[y:bottom, x:right] = prediction[0, 0, cy:cb, cx:cr]
    return result


def cpu_session(graph: Path, threads: int):
    import onnxruntime as ort
    if threads < 1:
        raise ValueError("positive CPU thread count required")
    with graph.open("rb") as stream:
        graph_bytes = stream.read(2 * 1024 * 1024 + 1)
    if not 0 < len(graph_bytes) <= 2 * 1024 * 1024 or hashlib.sha256(graph_bytes).hexdigest() != GRAPH_SHA256:
        raise ValueError("unreviewed STDF graph identity")
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(graph_bytes, sess_options=options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise RuntimeError("CPU-only provider contract failed")
    return session


def small_oracle(graph: Path, output: Path, width: int, height: int, case: str, threads: int = 2) -> dict:
    if width * height > 393_216:
        raise ValueError("whole-frame CPU oracle restricted to small shapes")
    tile_plan(width, height)
    _fresh(output)
    report = {"schemaVersion": 1, "status": "running", "case": case,
              "shape": [height, width], "graph_sha256": sha256(graph),
              "evidence_scope": "all output pixels versus small whole-frame CPU oracle; no GPU qualification"}
    try:
        session = cpu_session(graph, threads)
        report["actual_providers"] = session.get_providers()
        start = time.perf_counter()
        full = session.run(None, {"luma_frames": tensor(case, width, height)})[0][0, 0, :height, :width]
        tiled = tiled_prediction(session, case, width, height)
        original = visible_plane(case, width, height, 103)
        full.astype("<f4").tofile(output / "reference-y.f32")
        tiled.astype("<f4").tofile(output / "tiled-y.f32")
        quantize(full, original).tofile(output / "expected-y.u8")
        report.update(statistics(full, tiled, original))
        report.update({"status": "completed_cpu_oracle", "tile_count": len(tile_plan(width, height)),
                       "elapsed_seconds": time.perf_counter() - start,
                       "passed": report["maximum_absolute_error"] <= 3e-5 and report["maximum_8bit_difference"] <= 1})
    except Exception as error:
        report.update({"status": "failed", "passed": False, "error": str(error)})
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def large_consistency(graph: Path, output: Path, width: int, height: int, case: str,
                      sample_tiles: int | None = 6, threads: int = 2) -> dict:
    tiles = tile_plan(width, height)
    if sample_tiles is not None and sample_tiles < 1:
        raise ValueError("sample tile count must be positive")
    indices = (list(range(len(tiles))) if sample_tiles is None else
               sorted(set(np.linspace(0, len(tiles) - 1, min(sample_tiles, len(tiles)), dtype=int).tolist())))
    other = tile_plan(width, height, 96)
    _fresh(output)
    report = {"schemaVersion": 1, "status": "running", "shape": [height, width], "case": case,
              "graph_sha256": sha256(graph), "halos": [64, 96], "tiles_total": len(tiles),
              "tiles_selected": indices, "all_tiles_evaluated": len(indices) == len(tiles),
              "full_frame_oracle": False, "gpu_used": False,
              "evidence_scope": "CPU tile-policy consistency only; sampled tiles are not complete 4K evidence",
              "halo96_is_unqualified_research_comparator": True, "results": []}
    try:
        session = cpu_session(graph, threads)
        report["actual_providers"] = session.get_providers()
        for index in indices:
            report["active_tile_index"] = index
            outputs = []
            for halo, policy in zip((64, 96), (tiles[index], other[index]), strict=True):
                inferred = session.run(None, {"luma_frames": tensor(case, width, height, policy.source)})[0]
                if not np.all(np.isfinite(inferred)):
                    inferred.astype("<f4").tofile(output / f"failed-tile-{index}-halo{halo}-nonfinite.f32")
                    raise ValueError("nonfinite CPU tile prediction")
                cx, cy, cr, cb = policy.crop
                outputs.append(inferred[0, 0, cy:cb, cx:cr].copy())
            original = visible_plane(case, width, height, 103, extent=tiles[index].core)
            item = {"tile_index": index, "core": tiles[index].core,
                    **statistics(outputs[0], outputs[1], original)}
            report["results"].append(item)
            if item["maximum_absolute_error"] > 3e-5 or item["maximum_8bit_difference"] > 1:
                outputs[0].astype("<f4").tofile(output / f"failed-tile-{index}-halo64.f32")
                outputs[1].astype("<f4").tofile(output / f"failed-tile-{index}-halo96.f32")
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        report["status"] = "completed_cpu_tile_consistency"
        report["compared_pixels"] = sum(item["compared_pixels"] for item in report["results"])
        report["maximum_absolute_error"] = max(item["maximum_absolute_error"] for item in report["results"])
        report["maximum_8bit_difference"] = max(item["maximum_8bit_difference"] for item in report["results"])
        report["passed"] = all(item["maximum_absolute_error"] <= 3e-5 and
                               item["maximum_8bit_difference"] <= 1 for item in report["results"])
    except Exception as error:
        report.update({"status": "failed", "passed": False, "error": str(error)})
        raise
    finally:
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def compare_native(fixture: Path, actual: Path, expected_y: Path | None = None) -> dict:
    source, _ = read_fixture(fixture)
    center = source["frames"][3]
    metadata = json.loads((actual / "output.json").read_text(encoding="utf-8-sig"))
    expected_metadata = {"width": source["width"], "height": source["height"],
                         "center_index": center["index"], "center_pts": center["pts"],
                         "center_duration": center["duration"],
                         "timestamp_recovered": center["timestampRecovered"], "color": source["color"]}
    if any(metadata.get(key) != value for key, value in expected_metadata.items()):
        raise ValueError("native center metadata differs from exact input identity")
    results = []
    for plane, name in enumerate(("y", "u", "v")):
        reference = expected_y if plane == 0 and expected_y else fixture.parent / f"center-{plane}.u8"
        expected = np.fromfile(reference, dtype=np.uint8)
        output = np.fromfile(actual / f"output-{name}.u8", dtype=np.uint8)
        if output.shape != expected.shape:
            raise ValueError("native visible plane byte count differs")
        delta = np.abs(expected.astype(np.int16) - output.astype(np.int16))
        results.append({"plane": name, "compared_pixels": int(output.size),
                        "changed_pixels": int(np.count_nonzero(delta)), "maximum_byte_error": int(delta.max())})
    return {"protocol": PROTOCOL, "metadata_exact": True, "planes": results,
            "passed": all(item["changed_pixels"] == 0 for item in results)}


def write_native_fixture(graph: Path, output: Path, width: int, height: int, case: str,
                         oracle: str = "whole", threads: int = 2, strength: float = 1.0) -> dict:
    """Real CPU reference for STDF_TILED_RAW_PROTOCOL.md; every visible core.

    A large tiled reference checks native parity with this independent policy.
    It is deliberately not labelled whole-frame neural-network equivalence.
    The native checker owns its strided chroma/timestamp lifetime assertions;
    this luma protocol does not independently round-trip their metadata.
    """
    tiles = tile_plan(width, height)
    if oracle not in ("whole", "tiled") or not np.isfinite(strength) or not 0 < strength <= 1:
        raise ValueError("native reference requires whole/tiled oracle and strength in (0,1]")
    if oracle == "whole" and width * height > 393_216:
        raise ValueError("whole-frame CPU oracle restricted to small shapes")
    _fresh(output)
    report = {"schemaVersion": 1, "status": "running", "width": width, "height": height,
              "case": case, "graph_sha256": sha256(graph), "tile_policy": TILE_POLICY,
              "oracle": "cpu-whole-frame-global-pad8" if oracle == "whole" else
                        "cpu-independent-tiled-global-pad8",
              "whole_frame_oracle": oracle == "whole", "all_output_pixels": True,
              "gpu_used": False, "tiles_total": len(tiles), "tiles_completed": 0,
              "strength": strength, "metadata_roundtrip_in_this_protocol": False,
              "evidence_scope": "independent CPU reference; native execution and GPU qualification are separate"}
    def checkpoint():
        (output / "cpu-reference-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    checkpoint()
    try:
        session = cpu_session(graph, threads)
        report["actual_providers"] = session.get_providers()
        start = time.perf_counter()
        # Stream deterministic rows, never allocate a seven-frame UHD tensor.
        with (output / "input-y7.u8").open("wb") as raw:
            for index in range(100, 107):
                for y in range(0, height, 128):
                    visible_plane(case, width, height, index,
                                  extent=(0, y, width, min(height, y + 128))).tofile(raw)
        if oracle == "whole":
            full = session.run(None, {"luma_frames": tensor(case, width, height)})[0]
            if full.shape != (1, 1, ceil8(height), ceil8(width)) or not np.all(np.isfinite(full)):
                raise ValueError("whole-frame oracle output shape/nonfinite mismatch")
            visible = full[0, 0, :height, :width]
            visible.astype("<f4").tofile(output / "reference-y.f32")
            quantize(visible, visible_plane(case, width, height, 103), strength).tofile(output / "reference-y.u8")
            report["tiles_completed"] = len(tiles)
        else:
            reference = np.memmap(output / "reference-y.u8", dtype=np.uint8, mode="w+", shape=(height, width))
            try:
                for tile_index, tile in enumerate(tiles):
                    inferred = session.run(None, {"luma_frames": tensor(case, width, height, tile.source)})[0]
                    if inferred.shape != (1, 1, tile.padded[1], tile.padded[0]) or not np.all(np.isfinite(inferred)):
                        np.asarray(inferred, dtype="<f4").tofile(output / f"failed-tile-{tile_index}.f32")
                        raise ValueError("CPU tile prediction shape/nonfinite mismatch")
                    x, y, right, bottom = tile.core
                    cx, cy, cr, cb = tile.crop
                    reference[y:bottom, x:right] = quantize(inferred[0, 0, cy:cb, cx:cr],
                        visible_plane(case, width, height, 103, extent=tile.core), strength)
                    report["tiles_completed"] = tile_index + 1
                    if (tile_index + 1) % 16 == 0:
                        reference.flush()
                        checkpoint()
                reference.flush()
            finally:
                del reference
        files = {name: {"bytes": (output / name).stat().st_size, "sha256": sha256(output / name)}
                 for name in ("input-y7.u8", "reference-y.u8")}
        manifest = {"schemaVersion": 1, "width": width, "height": height, "case": case,
                    "graphSha256": GRAPH_SHA256, "tilePolicy": TILE_POLICY, "strength": strength,
                    "oracle": report["oracle"], "files": files,
                    "generatorSha256": sha256(Path(__file__)),
                    "graphSourcePath": str(graph.resolve()),
                    "allOutputPixels": True, "wholeFrameOracle": oracle == "whole"}
        # Publish the manifest last so a native runner cannot consume a partial reference.
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        report.update({"status": "completed_cpu_native_fixture", "compared_pixels": width * height,
                       "elapsed_seconds": time.perf_counter() - start, "files": files, "passed": True})
    except Exception as error:
        report.update({"status": "failed", "passed": False, "error": str(error)})
        raise
    finally:
        checkpoint()
    return report


def compare_native_luma(fixture: Path, actual: Path) -> dict:
    """Independently read every native output byte and cross-check its report."""
    manifest = json.loads((fixture / "manifest.json").read_text(encoding="utf-8-sig"))
    native = json.loads((actual / "report.json").read_text(encoding="utf-8-sig"))
    width, height = manifest["width"], manifest["height"]
    tiles = tile_plan(width, height)
    if (manifest["schemaVersion"] != 1 or manifest["graphSha256"] != GRAPH_SHA256 or
            manifest["tilePolicy"] != TILE_POLICY or manifest["oracle"] not in
            ("cpu-whole-frame-global-pad8", "cpu-independent-tiled-global-pad8")):
        raise ValueError("native fixture identity/oracle differs")
    for name, count in (("input-y7.u8", 7 * width * height), ("reference-y.u8", width * height)):
        path = fixture / name
        if (path.stat().st_size != count or manifest["files"][name]["bytes"] != count or
                sha256(path) != manifest["files"][name]["sha256"]):
            raise ValueError("native fixture byte count/SHA-256 differs")
    expected = np.fromfile(fixture / "reference-y.u8", dtype=np.uint8)
    result_path = actual / "native-y.u8"
    if result_path.stat().st_size != width * height:
        raise ValueError("native visible luma byte count differs")
    result = np.fromfile(result_path, dtype=np.uint8)
    delta = np.abs(expected.astype(np.int16) - result.astype(np.int16))
    max_error, changed, mean_error = int(delta.max()), int(np.count_nonzero(delta)), float(delta.mean())
    required = {"schemaVersion": 1, "graph_sha256": GRAPH_SHA256, "tile_policy": TILE_POLICY,
                "source_width": width, "source_height": height, "oracle": manifest["oracle"],
                "strength": manifest["strength"], "tiles_planned": len(tiles), "tiles_completed": len(tiles),
                "critical_compute_verified": True, "cpu_image_fallback": False,
                "whole_frame_oracle_present": manifest["oracle"] == "cpu-whole-frame-global-pad8",
                "max_8bit_difference": max_error, "changed_samples": changed, "passed": max_error <= 1}
    if (any(native.get(key) != value for key, value in required.items()) or
            not np.isclose(native.get("mean_8bit_absolute_error", np.nan), mean_error, rtol=0, atol=1e-12)):
        raise ValueError("native report disagrees with independent complete-output readback")
    return {"schemaVersion": 1, "passed": max_error <= 1, "compared_pixels": width * height,
            "max_8bit_difference": max_error, "changed_samples": changed,
            "mean_8bit_absolute_error": mean_error, "all_output_pixels": True,
            "whole_frame_oracle": manifest["oracle"] == "cpu-whole-frame-global-pad8",
            "oracle": manifest["oracle"], "native_output_sha256": sha256(result_path),
            "metadata_roundtrip_in_this_protocol": False,
            "scope": "all-byte native/CPU luma parity; native checker separately asserts chroma/timestamps"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("fixture", "oracle", "large", "compare", "native-fixture", "native-compare"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--graph", type=Path)
    parser.add_argument("--width", type=int, default=519)
    parser.add_argument("--height", type=int, default=263)
    parser.add_argument("--case", choices=CASES, default=CASES[0])
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--sample-tiles", type=int, default=6)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--actual", type=Path)
    parser.add_argument("--expected-y", type=Path)
    parser.add_argument("--oracle", choices=("whole", "tiled"), default="whole")
    parser.add_argument("--strength", type=float, default=1.0)
    args = parser.parse_args()
    if args.mode in ("compare", "native-compare"):
        if not args.fixture or not args.actual:
            parser.error("compare requires --fixture and --actual")
        report = (compare_native_luma(args.fixture, args.actual) if args.mode == "native-compare" else
                  compare_native(args.fixture, args.actual, args.expected_y))
    else:
        if not args.output or (args.mode != "fixture" and not args.graph):
            parser.error("fresh --output and (for inference) --graph required")
        if args.mode == "fixture":
            report = write_fixture(args.output, args.width, args.height, args.case)
        elif args.mode == "native-fixture":
            report = write_native_fixture(args.graph, args.output, args.width, args.height, args.case,
                                          args.oracle, args.threads, args.strength)
        elif args.mode == "oracle":
            report = small_oracle(args.graph, args.output, args.width, args.height, args.case, args.threads)
        else:
            report = large_consistency(args.graph, args.output, args.width, args.height, args.case,
                                       args.sample_tiles, args.threads)
    print(json.dumps({key: value for key, value in report.items() if key not in ("frames", "tiles", "results")}, indent=2))
    if report.get("passed") is False:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

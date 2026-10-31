"""Local STDF video-luma quality study; no downloads, uploads or runtime Python.

A recorded clip supplies motion/texture, not camera ground truth. Its decoded
luma is clamped into a controlled limited-range grayscale proxy reference before
software re-encoding. This intentionally makes no source-color, chroma, HDR,
restoration-superiority or platform-transcode claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

import numpy as np

from stdf_r3_cpu_quality import metrics, scene_window
from stdf_tiling_reference import (GRAPH_SHA256, TILE_POLICY, compare_native_luma,
                                   cpu_session, quantize, tile_plan)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_sequence(sequence: np.ndarray) -> tuple[int, int, int]:
    if sequence.dtype != np.uint8 or sequence.ndim != 3:
        raise ValueError("Expected owned or view uint8 [frames,height,width] luma")
    count, height, width = sequence.shape
    if not 7 <= count <= 120 or min(width, height) < 32 or width % 2 or height % 2:
        raise ValueError("Use 7..120 frames and even dimensions >=32")
    tile_plan(width, height)
    return count, height, width


def predict_sequence(session, sequence: np.ndarray, cuts: list[int]) -> np.ndarray:
    """Exact source-index windows, globally padded tiles, visible float output.

    This is an independent CPU oracle, not native adapter/GPU evidence. Keep
    the per-tile float tensor bounded and never pad an internal source edge.
    """
    count, height, width = validate_sequence(sequence)
    tiles = tile_plan(width, height)
    result = np.empty(sequence.shape, dtype=np.float32)
    for center in range(count):
        indices = scene_window(center, count, cuts)
        for tile in tiles:
            sx, sy, right, bottom = tile.source
            window = sequence[indices, sy:bottom, sx:right].astype(np.float32) / 255.0
            padded_width, padded_height = tile.padded
            window = np.pad(window, ((0, 0), (0, padded_height - (bottom - sy)),
                                    (0, padded_width - (right - sx))), mode="edge")
            prediction = session.run(None, {"luma_frames": window[None]})[0]
            if prediction.shape != (1, 1, padded_height, padded_width) or not np.isfinite(prediction).all():
                raise ValueError("CPU STDF output is incomplete or non-finite")
            x, y, cr, cb = tile.core
            cx, cy, crop_right, crop_bottom = tile.crop
            result[center, y:cb, x:cr] = prediction[0, 0, cy:crop_bottom, cx:crop_right]
    return result


def blend_sequence(prediction: np.ndarray, original: np.ndarray, strength: float) -> np.ndarray:
    validate_sequence(original)
    if prediction.shape != original.shape:
        raise ValueError("Prediction/reference dimensions differ")
    return quantize(prediction, original, strength)


def comparison(output: np.ndarray, reference: np.ndarray, source: np.ndarray) -> dict:
    validate_sequence(reference)
    if (output.shape != reference.shape or source.shape != reference.shape or
            output.dtype != np.uint8 or source.dtype != np.uint8):
        raise ValueError("All compared visible uint8 pixels must align")
    baseline = metrics(source, reference)
    measured = metrics(output, reference)
    per_frame = []
    for actual, expected, before in zip(output, reference, source):
        mse = float(np.mean((actual.astype(np.float64) - expected) ** 2))
        old_mse = float(np.mean((before.astype(np.float64) - expected) ** 2))
        per_frame.append({"source_mse": old_mse, "restored_mse": mse,
                          "delta_psnr_db": float(10 * np.log10(old_mse / mse))
                          if mse > 0 and old_mse > 0 else None})
    return {"input": baseline, "output": measured,
            "delta_psnr_y_db": measured["psnr_y_db"] - baseline["psnr_y_db"]
            if measured["psnr_y_db"] is not None and baseline["psnr_y_db"] is not None else None,
            "frames_improved_mse": sum(x["restored_mse"] < x["source_mse"] for x in per_frame),
            "frames_worsened_mse": sum(x["restored_mse"] > x["source_mse"] for x in per_frame),
            "pixels_changed": int(np.count_nonzero(output != source)), "per_frame": per_frame}


def run(command: list[str], *, payload: bytes | None = None) -> bytes:
    completed = subprocess.run(command, input=payload, capture_output=True, timeout=600)
    if completed.returncode:
        raise RuntimeError(f"Local subprocess failed ({completed.returncode}): "
                           f"{completed.stderr.decode('utf-8', errors='replace')[-6000:]}")
    return completed.stdout


def write_native_fixture(directory: Path, sequence: np.ndarray, prediction: np.ndarray,
                         center: int, cuts: list[int], graph: Path) -> dict:
    count, height, width = validate_sequence(sequence)
    if not 0 <= center < count or prediction.shape != sequence.shape:
        raise ValueError("Invalid native quality-reference center")
    if digest(graph) != GRAPH_SHA256:
        raise ValueError("Native quality fixture requires the pinned STDF graph")
    directory.mkdir(parents=True, exist_ok=False)
    sequence[scene_window(center, count, cuts)].tofile(directory / "input-y7.u8")
    quantize(prediction[center], sequence[center], 1.0).tofile(directory / "reference-y.u8")
    files = {name: {"bytes": (directory / name).stat().st_size, "sha256": digest(directory / name)}
             for name in ("input-y7.u8", "reference-y.u8")}
    manifest = {"schemaVersion": 1, "width": width, "height": height,
                "case": "recorded-luma-controlled-proxy", "graphSha256": GRAPH_SHA256,
                "tilePolicy": TILE_POLICY, "strength": 1.0,
                "oracle": "cpu-independent-tiled-global-pad8", "files": files,
                "sourceCenterIndex": center, "allOutputPixels": True,
                "wholeFrameOracle": False, "metadataRoundtrip": False}
    # Publish last; the native checker must never consume a partial oracle.
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--frames", type=int, default=15)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--cuts", type=int, nargs="*", default=[])
    parser.add_argument("--native-checker", type=Path)
    parser.add_argument("--runtime", type=Path)
    args = parser.parse_args()
    if bool(args.native_checker) != bool(args.runtime):
        parser.error("--native-checker and --runtime must be supplied together")
    if (not 7 <= args.frames <= 120 or min(args.width, args.height) < 32 or
            args.width % 2 or args.height % 2 or args.width * args.height * args.frames > 32 * 1024 * 1024):
        parser.error("Bounded study requires even dimensions >=32, 7..120 frames and <=32 Mi luma samples")
    tile_plan(args.width, args.height)
    scene_window(0, args.frames, args.cuts)
    if digest(args.graph) != GRAPH_SHA256:
        raise ValueError("Quality study requires the qualified graph hash")
    args.output.mkdir(parents=True, exist_ok=False)
    source_hash = digest(args.video)
    command = [str(args.ffmpeg), "-nostdin", "-v", "error", "-i", str(args.video),
               "-map", "0:v:0", "-vf", f"scale={args.width}:{args.height}:flags=lanczos,format=yuv420p,extractplanes=y",
               "-frames:v", str(args.frames), "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
    decoded = run(command)
    if len(decoded) != args.frames * args.width * args.height:
        raise ValueError("Selected clip did not decode the requested bounded frame count")
    reference = np.frombuffer(decoded, dtype=np.uint8).reshape(args.frames, args.height, args.width)
    reference = np.clip(reference, 16, 235).astype(np.uint8)
    raw = b"".join(frame.tobytes() + bytes([128]) * (args.width * args.height // 2) for frame in reference)
    (args.output / "proxy-reference.yuv").write_bytes(raw)
    session = cpu_session(args.graph, args.threads)
    configs = [("proxy-clean", None, []),
               ("h264-80kbps", "libopenh264", ["-b:v", "80k", "-allow_skip_frames", "0"]),
               ("hevc-qp37", "libkvazaar", ["-kvazaar-params", "qp=37:threads=2:owf=0"])]
    report = {"schemaVersion": 1, "status": "running", "source_sha256": source_hash,
              "graph_sha256": digest(args.graph), "tool_sha256": digest(Path(__file__)),
              "ffmpeg_sha256": digest(args.ffmpeg), "tile_policy": TILE_POLICY,
              "actual_cpu_providers": session.get_providers(), "source_decode_command": command,
              "proxy": {"width": args.width, "height": args.height, "frames": args.frames,
                        "cuts": args.cuts, "encoded_rate": "25/1", "range_clamped": [16, 235],
                        "chroma": "constant 128", "reference_sha256": digest(args.output / "proxy-reference.yuv")},
              "cases": [], "limits": [
                  "Recorded luma and controlled added compression; reference is a decoded proxy, not camera truth",
                  "Grayscale study does not validate source colors, chroma processing, HDR or audio/video timeline",
                  "25 FPS is a controlled encoding cadence, not preservation of the source timeline",
                  "Cuts are supplied explicitly; inferred scene detection and natural footage perceptual quality remain separate",
                  "CPU metrics compare every visible pixel; native proof covers only the explicitly selected center if requested",
                  "No claim of Topaz equivalence, platform upload/transcode behavior, strongest model or general quality improvement"]}
    path = args.output / "quality.json"

    def save() -> None:
        path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    save()
    try:
        for name, encoder, options in configs:
            source = reference
            commands = []
            if encoder:
                clip = args.output / f"{name}.mkv"
                encode = [str(args.ffmpeg), "-nostdin", "-v", "error", "-n", "-f", "rawvideo",
                          "-pixel_format", "yuv420p", "-video_size", f"{args.width}x{args.height}",
                          "-framerate", "25", "-i", "-", "-vf",
                          "setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709",
                          "-an", "-c:v", encoder, *options, str(clip)]
                run(encode, payload=raw)
                decode = [str(args.ffmpeg), "-nostdin", "-v", "error", "-i", str(clip),
                          "-map", "0:v:0", "-vf", "extractplanes=y", "-frames:v", str(args.frames),
                          "-fps_mode", "passthrough", "-f", "rawvideo", "-"]
                unpack = run(decode)
                commands = [encode, decode]
                if len(unpack) != reference.size:
                    raise ValueError("Software codec changed the controlled decoded frame count")
                source = np.frombuffer(unpack, dtype=np.uint8).reshape(reference.shape)
            start = time.perf_counter()
            prediction = predict_sequence(session, source, args.cuts)
            item = {"name": name, "cpu_seconds": time.perf_counter() - start,
                    "commands": commands,
                    "decoded_luma_sha256": hashlib.sha256(source.tobytes()).hexdigest(),
                    "strengths": {str(strength): comparison(blend_sequence(prediction, source, strength), reference, source)
                                  for strength in (0.0, 0.25, 0.5, 1.0)}}
            if args.native_checker:
                center = args.frames // 2
                fixture, actual = args.output / f"{name}-fixture", args.output / f"{name}-native"
                write_native_fixture(fixture, source, prediction, center, args.cuts, args.graph)
                native_command = [str(args.native_checker), str(args.graph), str(args.runtime), str(fixture), str(actual)]
                (args.output / f"{name}-native-stdout.txt").write_bytes(run(native_command))
                verification = compare_native_luma(fixture, actual)
                if not verification["passed"]:
                    raise ValueError("Native real-luma center failed independent all-byte comparison")
                item["native_center"] = {"index": center, "checker_sha256": digest(args.native_checker),
                                         "command": native_command, "verification": verification,
                                         "report": json.loads((actual / "report.json").read_text(encoding="utf-8"))}
            report["cases"].append(item)
            save()
            print(json.dumps({"case": name, "cpu_seconds": item["cpu_seconds"],
                              "strength_delta_psnr": {key: value["delta_psnr_y_db"] for key, value in item["strengths"].items()}}), flush=True)
        if digest(args.video) != source_hash:
            raise ValueError("Input source identity changed during local evaluation")
        report.update(status="completed_local_real_luma_study", source_unchanged=True)
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        save()


if __name__ == "__main__":
    main()

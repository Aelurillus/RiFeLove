#!/usr/bin/env python3
"""Exercise automatic detail with local NAFNet, isolated media, and a crash resume."""

from __future__ import annotations

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time


WIDTH, HEIGHT, SOURCE_FRAMES, OUTPUT_FRAMES = 384, 256, 8, 16
FRAME_BYTES = WIDTH * HEIGHT * 3


def run(*args: object, timeout: int = 300) -> bytes:
    command = [str(value) for value in args]
    completed = subprocess.run(command, capture_output=True, timeout=timeout)
    if completed.returncode:
        raise RuntimeError(f"{command[0]} failed ({completed.returncode}): "
                           + completed.stderr[-4000:].decode("utf-8", "replace"))
    return completed.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def make_source(ffmpeg: Path, destination: Path) -> None:
    command = [str(ffmpeg), "-v", "error", "-nostdin", "-f", "rawvideo",
               "-pix_fmt", "yuv444p", "-s:v", f"{WIDTH}x{HEIGHT}", "-r", "30",
               "-i", "pipe:0", "-frames:v", str(SOURCE_FRAMES),
               "-c:v", "h264_nvenc", "-preset", "p4", "-rc", "constqp", "-qp", "0",
               "-pix_fmt", "yuv420p",
               "-color_primaries", "bt709", "-color_trc", "bt709",
               "-colorspace", "bt709", "-color_range", "tv",
               "-bsf:v", "h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1",
               str(destination)]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdin is not None and process.stderr is not None
    chroma = bytes([128]) * (WIDTH * HEIGHT)
    weights = (1, 2, 3, 4, 3, 2, 1)
    for index in range(SOURCE_FRAMES):
        if index >= 4:
            luma = bytes([220]) * (WIDTH * HEIGHT)
        else:
            luma = bytearray(WIDTH * HEIGHT)
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    value = sum((192 if ((min(WIDTH - 1, max(0, x + index + offset)) // 32
                                           + y // 32) & 1) else 64) * weights[offset + 3]
                                for offset in range(-3, 4))
                    luma[y * WIDTH + x] = value // 16
        process.stdin.write(luma + chroma + chroma)
    process.stdin.close()
    error = process.stderr.read()
    if process.wait() != 0:
        raise RuntimeError("Could not encode synthetic BT.709 source: "
                           + error[-4000:].decode("utf-8", "replace"))


def frames(ffmpeg: Path, path: Path) -> list[bytes]:
    raw = run(ffmpeg, "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0",
              "-pix_fmt", "rgb24", "-f", "rawvideo", "-")
    if len(raw) % FRAME_BYTES:
        raise RuntimeError(f"Unexpected decoded frame extent: {path}")
    return [raw[offset:offset + FRAME_BYTES] for offset in range(0, len(raw), FRAME_BYTES)]


def compare_decoded_frames(reference: list[bytes], recovered: list[bytes]) -> dict:
    """Enforce a narrow empirical tolerance; never claim GPU bit-exactness."""
    if len(reference) != len(recovered) or not reference:
        raise ValueError("Decoded sequences have different or empty frame counts")
    changed = maximum = total = 0
    for expected, actual in zip(reference, recovered):
        if len(expected) != len(actual) or not expected:
            raise ValueError("Decoded frame byte extents differ or are empty")
        total += len(expected)
        for left, right in zip(expected, actual):
            delta = abs(left - right)
            changed += delta != 0
            maximum = max(maximum, delta)
    # Multiple local uninterrupted CUDA runs showed sparse differences up to
    # three levels. Bound both magnitude and frequency, rather than treating
    # all differing pixels as proof of recovery failure or allowing drift.
    if maximum > 3 or changed * 10000 > total:
        raise ValueError("Recovered decoded pixels exceed the local three-level / 0.01% tolerance")
    return {"exact": changed == 0, "maximum_channel_delta": maximum,
            "changed_channels": changed, "total_channels": total,
            "tolerance_maximum_channel_delta": 3, "tolerance_changed_fraction": 0.0001}


def timeline(ffprobe: Path, path: Path) -> list[Fraction]:
    probe = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames",
                           "-show_streams", "-show_frames",
                           "-show_entries", "stream=width,height,r_frame_rate,nb_read_frames:"
                           "frame=best_effort_timestamp_time", "-of", "json", path))
    stream = probe["streams"][0]
    if (stream["width"], stream["height"], stream["r_frame_rate"],
            int(stream["nb_read_frames"])) != (WIDTH, HEIGHT, "60/1", OUTPUT_FRAMES):
        raise RuntimeError(f"Output rate, size, or frame count changed: {path}")
    pts = [Fraction(frame["best_effort_timestamp_time"]) for frame in probe["frames"]]
    if len(pts) != OUTPUT_FRAMES or pts[0] != 0 or any(pts[i] <= pts[i - 1] for i in range(1, len(pts))):
        raise RuntimeError(f"Output PTS are incomplete or unordered: {path}")
    if any(abs(value - Fraction(index, 60)) > Fraction(1, 1000) for index, value in enumerate(pts)):
        raise RuntimeError(f"Output PTS drift from exact 60 FPS beyond Matroska timebase: {path}")
    return pts


def checkpoint(database: Path) -> tuple[str, list[tuple]]:
    with sqlite3.connect(f"file:{database.as_posix()}?mode=ro", uri=True) as connection:
        manifest = connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()
        segments = connection.execute(
            "SELECT segment_index, artifact_path, sha256, byte_count "
            "FROM task_checkpoint_segments ORDER BY segment_index").fetchall()
    if manifest is None:
        raise RuntimeError("Checkpoint manifest is missing")
    return manifest[0], segments


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "ffmpeg", "ffprobe", "validator", "rife", "deblur", "runtime", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    if root.exists():
        raise RuntimeError("Use a fresh isolated output directory; evidence is never overwritten")
    root.mkdir(parents=True)
    source = root / "source.mp4"
    make_source(args.ffmpeg, source)
    source_digest = sha256(source)
    deblur_descriptor_digest = sha256(args.deblur / "model.json")
    model = json.loads((args.deblur / "model.json").read_text(encoding="utf-8"))
    print("Synthetic source ready; running baseline and automatic NAFNet queues", flush=True)

    def runner(mode: str) -> tuple[dict, float]:
        started = time.monotonic()
        output = run(args.runner, mode, root, source, args.rife, args.runtime, args.deblur)
        return json.loads(output.decode("utf-8").splitlines()[-1]), time.monotonic() - started

    baseline, baseline_wall = runner("baseline")
    reference, reference_wall = runner("reference")
    detail = reference["automaticDetailRestoration"]
    if (not detail["active"] or detail["analyzedSourceFrames"] != SOURCE_FRAMES
            or detail["confidentSourceFrames"] <= 0 or detail["conditionedOutputFrames"] <= 0):
        raise RuntimeError(f"Automatic detail did not produce real positive requests: {detail}")
    if (reference["restorationModelId"] != model["id"]
            or reference["sourceDecodeAcceleration"] != "software"
            or reference["validation"]["decodedFrames"] != OUTPUT_FRAMES
            or reference["outputFrames"] != OUTPUT_FRAMES):
        raise RuntimeError("Real model identity, source replay, or output validation is missing")
    baseline_pixels = frames(args.ffmpeg, root / "baseline.mkv")
    reference_pixels = frames(args.ffmpeg, root / "reference.mkv")
    if len(baseline_pixels) != OUTPUT_FRAMES or len(reference_pixels) != OUTPUT_FRAMES:
        raise RuntimeError("Baseline or automatic output lost frames")
    changed_before_cut = [index for index in range(8) if baseline_pixels[index] != reference_pixels[index]]
    changed_after_cut = [index for index in range(8, OUTPUT_FRAMES)
                         if baseline_pixels[index] != reference_pixels[index]]
    if not changed_before_cut or changed_after_cut:
        raise RuntimeError(f"Positive model use or low-confidence/scene bypass failed: "
                           f"before={changed_before_cut}, after={changed_after_cut}")
    reference_pts = timeline(args.ffprobe, root / "reference.mkv")
    timeline(args.ffprobe, root / "baseline.mkv")
    print(f"Reference used NAFNet on {detail['conditionedOutputFrames']} output frames; "
          "starting interrupted queue", flush=True)

    marker = root / "paused.marker"
    with (root / "hold.stdout.txt").open("wb") as stdout, (root / "hold.stderr.txt").open("wb") as stderr:
        holder = subprocess.Popen([str(value) for value in (
            args.runner, "hold", root, source, args.rife, args.runtime, args.deblur)],
            stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 180
            while not marker.exists() and time.monotonic() < deadline:
                if holder.poll() is not None:
                    raise RuntimeError("Hold worker exited before checkpoint: "
                                       + (root / "hold.stderr.txt").read_text(errors="replace")[-4000:])
                time.sleep(0.1)
            if not marker.exists():
                raise TimeoutError("NAFNet worker did not pause after a committed checkpoint")
            saved_manifest, saved_segments = checkpoint(root / "resume.sqlite")
            saved = json.loads(saved_manifest)
            if (len(saved_segments) < 1 or saved["framesPerSegment"] != 2
                    or saved["job"]["schemaVersion"] < 13
                    or saved["restorationRuntime"]["automaticControl"] !=
                    saved["job"]["automaticDetailRestoration"]
                    or saved["job"]["expectedRestorationDescriptorSha256"] != deblur_descriptor_digest
                    or saved["restorationRuntime"]["modelId"] != model["id"]):
                raise RuntimeError("Checkpoint did not pin the model, analyzer, and replay settings")
        finally:
            holder.terminate()
            try:
                holder.wait(timeout=10)
            except subprocess.TimeoutExpired:
                holder.kill()
                holder.wait(timeout=10)
    resumed, recovery_wall = runner("recover")
    resumed_manifest, resumed_segments = checkpoint(root / "resume.sqlite")
    if (resumed_manifest != saved_manifest or resumed_segments[:len(saved_segments)] != saved_segments
            or resumed["reusedFrames"] < 2
            or resumed["reusedFrames"] + resumed["processedFrames"] != OUTPUT_FRAMES):
        raise RuntimeError("Resume changed verified segment identity or output accounting")
    resumed_pixels = frames(args.ffmpeg, root / "resumed.mkv")
    if timeline(args.ffprobe, root / "resumed.mkv") != reference_pts:
        raise RuntimeError("Recovered timestamps differ from continuous execution")
    pixel_comparison = compare_decoded_frames(reference_pixels, resumed_pixels)
    if sha256(source) != source_digest:
        raise RuntimeError("Synthetic source changed during processing")

    first_rgb = root / "first-source.rgb"
    first_rgb.write_bytes(frames(args.ffmpeg, source)[0])
    validator_output = root / "nafnet-direct.rgb"
    profiled = json.loads(run(args.validator, args.deblur, args.runtime, first_rgb, WIDTH, HEIGHT,
                              validator_output, "cuda", 0.2, timeout=180).decode("utf-8"))
    if profiled["provider_events"].get("CUDAExecutionProvider", 0) <= 0:
        raise RuntimeError(f"Real NAFNet graph did not produce CUDA provider events: {profiled}")

    report = {
        "status": "passed",
        "source_sha256": source_digest,
        "deblur_graph_sha256": model["sha256"],
        "restoration_model": reference["restorationModelId"],
        "restoration_backend": reference["restorationBackend"],
        "source_decoder": reference["sourceDecodeAcceleration"],
        "frames": OUTPUT_FRAMES,
        "fps": "60/1",
        "end_exclusive": reference["endExclusive"],
        "changed_output_frames_before_cut": changed_before_cut,
        "changed_output_frames_after_cut": changed_after_cut,
        "automatic": detail,
        "checkpoint_segments_before_crash": len(saved_segments),
        "reused_frames": resumed["reusedFrames"],
        "continuous_decoded_sha256": hashlib.sha256(b"".join(reference_pixels)).hexdigest(),
        "recovered_decoded_sha256": hashlib.sha256(b"".join(resumed_pixels)).hexdigest(),
        "recovered_pixel_comparison": pixel_comparison,
        "cuda_provider_events_direct_probe": profiled["provider_events"]["CUDAExecutionProvider"],
        "direct_model_seconds": profiled["elapsed_seconds"],
        "baseline_wall_seconds": baseline_wall,
        "automatic_wall_seconds": reference_wall,
        "recovery_wall_seconds": recovery_wall,
        "pipeline_restoration_seconds": reference["timings"]["restorationSeconds"],
        "limitations": "Eight synthetic SDR frames, one GPU/model, CUDA backend; no perceptual quality, "
                       "false-positive, long-video, or TensorRT qualification.",
    }
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()

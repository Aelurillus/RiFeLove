"""Validate local RIFE + FastDVDnet queue crash/replay using existing packs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("queue", "ffmpeg", "rife_pack", "temporal_pack", "runtime", "input_rgb", "output"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    args = parser.parse_args()
    for key, value in vars(args).items():
        setattr(args, key, value.resolve())
    args.output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join([str(args.ffmpeg.parent), str(args.runtime), environment.get("PATH", "")])

    def run(command: list[str], timeout: int = 120) -> subprocess.CompletedProcess:
        result = subprocess.run([str(item) for item in command], env=environment, capture_output=True, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"{command[0]} failed: {result.stderr.decode('utf-8', errors='replace')[-4000:]}")
        return result

    # Repeat the six-frame noisy/clean cut fixture to cover several scenes and
    # ensure the worker reaches a committed segment plus a live writer.
    raw = args.input_rgb.read_bytes()
    if len(raw) != 6 * 192 * 128 * 3:
        raise ValueError("Expected the existing 192x128 six-frame RGB evidence clip")
    source_rgb = args.output / "source.rgb"
    source_rgb.write_bytes(raw * 2)
    source = args.output / "source.mkv"
    run([args.ffmpeg, "-v", "error", "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "192x128",
         "-framerate", "30", "-i", source_rgb, "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.4",
         "-vf", "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited",
         "-c:v", "ffv1", "-pix_fmt", "yuv444p", "-c:a", "flac", "-frames:v", "12", source])
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    reference = args.output / "reference"
    interrupted = args.output / "interrupted"
    reference.mkdir()
    interrupted.mkdir()
    reference_report = reference / "result.json"
    run([args.queue, "automatic-denoise-run", reference, source, args.rife_pack, args.runtime, reference_report, args.temporal_pack])
    with sqlite3.connect(reference / "native-queue.sqlite") as connection:
        reference_manifest = json.loads(connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0])
    marker = interrupted / "paused.txt"
    with (interrupted / "holder.stdout.txt").open("wb") as stdout, (interrupted / "holder.stderr.txt").open("wb") as stderr:
        # Only this test-owned child is terminated. No user application is killed.
        holder = subprocess.Popen([str(item) for item in [args.queue, "automatic-denoise-hold", interrupted, source,
                                  args.rife_pack, args.runtime, marker, args.temporal_pack]], env=environment, stdout=stdout, stderr=stderr)
        try:
            deadline = time.monotonic() + 90
            while not marker.exists():
                if holder.poll() is not None:
                    raise RuntimeError("Test worker exited before checkpoint pause: " + (interrupted / "holder.stderr.txt").read_text(errors="replace")[-4000:])
                if time.monotonic() >= deadline:
                    raise TimeoutError("Test worker did not acknowledge a safe checkpoint pause")
                time.sleep(0.05)
            database = interrupted / "native-queue.sqlite"
            with sqlite3.connect(database) as connection:
                saved_manifest = connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0]
                saved = json.loads(saved_manifest)
                configuration = json.loads(connection.execute("SELECT configuration FROM task_details").fetchone()[0])
                segments = connection.execute("SELECT COUNT(*), MAX(end_output_frame) FROM task_checkpoint_segments").fetchone()
                saved_segments = connection.execute(
                    "SELECT segment_index,artifact_path,sha256,byte_count FROM task_checkpoint_segments ORDER BY segment_index").fetchall()
                saved_task_id, state = connection.execute("SELECT id,state FROM tasks").fetchone()
                assert state == "paused"
            assert saved["job"]["automaticTemporalDenoise"] == configuration["automaticTemporalDenoise"]
            assert configuration["schemaVersion"] == 9 and configuration["automaticTemporalDenoise"] is not None
            assert segments[0] > 0 and 0 < segments[1] < 24
            media_identity = saved["mediaPipelineIdentity"]
            assert saved["schemaVersion"] == 2 and media_identity["schemaVersion"] == 2
            processing = media_identity["processingPipeline"]
            canonical = "algorithm=" + processing["algorithmVersion"] + "\n"
            canonical += "".join(f"{name}={digest}\n" for name, digest in sorted(processing["sourceFiles"].items()))
            assert hashlib.sha256(canonical.encode("utf-8")).hexdigest() == processing["sourceSha256"]
            for name, digest in processing["sourceFiles"].items():
                assert hashlib.sha256((Path(__file__).resolve().parents[2] / name).read_bytes()).hexdigest() == digest, name
            assert media_identity == reference_manifest["mediaPipelineIdentity"], "Fresh tasks loaded different media implementations"
            writer_source = Path(__file__).resolve().parents[2] / "src/media/video_writer.cpp"
            assert media_identity["writerSourceSha256"] == hashlib.sha256(writer_source.read_bytes()).hexdigest()
            for library in ("avcodec", "avformat", "avutil", "swscale"):
                files = list(args.ffmpeg.parent.glob(library + "-*.dll"))
                assert len(files) == 1, f"Expected one local FFmpeg SDK DLL for {library}"
                binary = files[0].read_bytes()
                assert media_identity["libraries"][library]["binarySha256"] == hashlib.sha256(binary).hexdigest(), library
                assert media_identity["libraries"][library]["binarySize"] == len(binary), library
            (args.output / "saved-checkpoint-manifest.json").write_text(saved_manifest, encoding="utf-8")
            holder.kill()
            holder.wait(timeout=10)
        finally:
            if holder.poll() is None:
                holder.kill()
                holder.wait(timeout=10)
    resumed_report = interrupted / "recovered-result.json"
    run([args.queue, "automatic-denoise-recover", interrupted, source, args.rife_pack, args.runtime, resumed_report])
    uninterrupted_video = reference / "automatic-output.mkv"
    resumed_video = interrupted / "crash-output.mkv"

    def frame_hashes(path: Path) -> bytes:
        return run([args.ffmpeg, "-v", "error", "-i", path, "-map", "0:v:0", "-f", "framemd5", "-"]).stdout

    def pcm(path: Path) -> bytes:
        return run([args.ffmpeg, "-v", "error", "-i", path, "-map", "0:a:0", "-f", "s16le", "-"]).stdout

    assert frame_hashes(uninterrupted_video) == frame_hashes(resumed_video), "Resumed decoded pixels or PTS differ"
    assert pcm(source) == pcm(uninterrupted_video) == pcm(resumed_video), "Audio samples differ"
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_hash
    first = json.loads(reference_report.read_text())
    resumed = json.loads(resumed_report.read_text())
    assert first["outputFrames"] == resumed["outputFrames"] == 24
    assert first["endExclusive"] == resumed["endExclusive"]
    assert resumed["reusedFrames"] == segments[1] and resumed["processedFrames"] + resumed["reusedFrames"] == 24
    assert first["sourceDecodeAcceleration"] == resumed["sourceDecodeAcceleration"] == "software"
    assert first["automaticGaussianDenoise"] == resumed["automaticGaussianDenoise"]
    with sqlite3.connect(interrupted / "native-queue.sqlite") as connection:
        assert connection.execute("SELECT state FROM tasks").fetchone()[0] == "completed"
        assert connection.execute("SELECT manifest_json FROM task_checkpoint_manifests").fetchone()[0] == saved_manifest
        recovered_segments = connection.execute(
            "SELECT segment_index,artifact_path,sha256,byte_count FROM task_checkpoint_segments ORDER BY segment_index").fetchall()
        assert recovered_segments[:len(saved_segments)] == saved_segments, "Committed checkpoint records were rewritten"
    for _, relative_path, digest, size in saved_segments:
        # The journal stores task-directory-relative segment paths; verify the
        # reused bytes as well as the unchanged SQLite record after recovery.
        segment_path = interrupted / "crash-output.mkv.rifelove-checkpoints" / saved_task_id / relative_path
        assert segment_path.stat().st_size == size
        assert hashlib.sha256(segment_path.read_bytes()).hexdigest() == digest
    summary = {"schemaVersion": 1, "synthetic_only": True, "success": True, "backend": first["backend"],
               "temporal_backend": first["temporalRestorationBackend"], "output_frames": 24,
               "reused_frames": resumed["reusedFrames"], "processed_frames": resumed["processedFrames"],
               "decoded_frame_pixels_and_pts_equal": True, "audio_pcm_equal": True, "source_preserved": True,
               "end_exclusive": first["endExclusive"], "automatic": first["automaticGaussianDenoise"],
               "saved_automatic_identity": configuration["automaticTemporalDenoise"],
               "saved_media_identity": media_identity, "ffmpeg_reported_version": saved["mediaRuntime"],
               "media_identity_preserved": True, "media_library_bytes_match_ffmpeg_sdk": True,
               "committed_segments_reused_unchanged": len(saved_segments)}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

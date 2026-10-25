"""Exercise real GPU queue persistence, pause/cancel/retry and process death."""
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

cli, native, ffmpeg, ffprobe, pack, runtime, reports = (Path(p).resolve() for p in sys.argv[1:])

def run(exe, *args, code=0):
    result = subprocess.run([str(exe), *(str(a) for a in args)], capture_output=True, encoding="utf-8", timeout=120)
    assert result.returncode == code, (result.returncode, result.stdout, result.stderr)
    return result

def rows(database):
    return [json.loads(line) for line in run(cli, "queue-list", database).stdout.splitlines()]

with tempfile.TemporaryDirectory(prefix="rife-queue-", dir=cli.parent) as folder:
    root = Path(folder)
    source = root / "中文 队列源.mkv"
    run(ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=5:duration=0.6",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=0.6",
        "-vf", "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited", "-c:v", "ffv1", "-c:a", "flac", source)
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    basic = root / "basic"
    basic.mkdir()
    run(native, "basic", basic, source, pack, runtime, reports / "queue-workflow.json")
    assert [r["state"] for r in rows(basic / "native-queue.sqlite")] == ["completed", "completed", "completed", "cancelled", "cancelled", "queued"]
    for filename in ("first.mkv", "second.mkv", "third.mkv"):
        output = basic / filename
        info = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries", "stream=nb_read_frames", "-of", "json", output).stdout)
        assert info["streams"][0]["nb_read_frames"] == "36"
        def pcm(path):
            return subprocess.check_output([str(ffmpeg), "-v", "error", "-i", str(path), "-map", "0:a:0", "-f", "s16le", "-"], timeout=15)
        assert pcm(source) == pcm(output)
    assert not list(basic.glob("*.partial"))
    stress = root / "stress"
    stress.mkdir()
    run(native, "stress", stress, source, pack, runtime, reports / "queue-stress.json")
    reference_hashes = None
    for index in range(20):
        hashes = run(ffmpeg, "-v", "error", "-i", stress / f"stress-{index}.mkv", "-map", "0:v:0", "-f", "framemd5", "-").stdout
        if reference_hashes is None:
            reference_hashes = hashes
        assert hashes == reference_hashes, "Repeated CUDA sessions changed decoded frame pixels/timing"

    # CLI queue roundtrip, atomic batch import and source identity on retry.
    database = root / "cli.sqlite"
    output = root / "cli-out.mkv"
    first_id = json.loads(run(cli, "queue-add", database, source, output, "--fps", "60000/1001", "--model-dir", pack, "--runtime-dir", runtime, "--encoder", "ffv1").stdout)["ids"][0]
    exported = json.loads(run(cli, "queue-export", database).stdout)
    assert exported["jobs"][0]["configuration"]["targetRate"] == "60000/1001"
    batch = json.loads(json.dumps(exported))
    batch["jobs"] *= 2
    batch["jobs"][0]["output"] = str(root / "duplicate.mkv")
    plan = root / "plan.json"
    plan.write_text(json.dumps(batch), encoding="utf-8")
    run(cli, "queue-import", database, plan, code=2)
    assert len(rows(database)) == 1
    batch = json.loads(json.dumps(exported))
    batch["jobs"][0]["output"] = str(root / "imported.mkv")
    plan.write_text(json.dumps(batch), encoding="utf-8")
    run(cli, "queue-import", database, plan)
    result = run(cli, "queue-run", database, "--progress")
    assert [r["state"] for r in rows(database)] == ["completed", "completed"]
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert events[0]["event"] == "queue_started" and events[-1]["event"] == "queue_stopped"
    assert len([e for e in events if e["event"] == "task_completed"]) == 2
    run(cli, "queue-retry", database, first_id, code=2)
    snapshot = rows(database)[0]
    assert snapshot["identity"]["source"]["sha256"] == original
    assert snapshot["identity"]["model"]["sha256"] == snapshot["configuration"]["expectedModelSha256"]

    mutable = root / "mutable.mkv"
    mutable.write_bytes(source.read_bytes())
    changed_id = json.loads(run(cli, "queue-add", database, mutable, root / "unavailable.mkv", "--fps", "60", "--model-dir", pack, "--runtime-dir", runtime, "--encoder", "does_not_exist").stdout)["ids"][0]
    run(cli, "queue-run", database, code=2)
    assert rows(database)[-1]["identity"]["source"]["sha256"] == original
    with mutable.open("ab") as stream:
        stream.write(b"source changed")
    run(cli, "queue-retry", database, changed_id)
    run(cli, "queue-run", database, code=2)
    assert "Source SHA-256 changed" in rows(database)[-1]["error"]
    assert not (root / "unavailable.mkv").exists()

    # Terminate a writer with a committed segment plus a live partial, then
    # detect interruption and resume the original queue task.
    crash = root / "crash"
    crash.mkdir()
    crash_source = root / "崩溃恢复 11秒.mkv"
    run(ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=5:duration=11",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=11",
        "-vf", "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited", "-c:v", "ffv1", "-c:a", "flac", crash_source)
    crash_source_hash = hashlib.sha256(crash_source.read_bytes()).hexdigest()
    marker = crash / "paused.txt"
    # ORT may log enough startup/kernel diagnostics to fill Windows' small
    # anonymous pipe buffers. Drain to files while the process is paused.
    holder_stdout = (crash / "holder.stdout.log").open("wb")
    holder_stderr = (crash / "holder.stderr.log").open("wb")
    holder = subprocess.Popen([str(native), "hold", str(crash), str(crash_source), str(pack), str(runtime), str(marker)], stdout=holder_stdout, stderr=holder_stderr)
    try:
        deadline = time.monotonic() + 120
        while not marker.exists():
            progress = (crash / "hold-progress.txt").read_text(encoding="utf-8", errors="replace") if (crash / "hold-progress.txt").exists() else "no processing event"
            assert holder.poll() is None, ("last output frame: " + progress, (crash / "holder.stderr.log").read_text(encoding="utf-8", errors="replace"))
            assert time.monotonic() < deadline, "Hold worker failed to pause"
            time.sleep(0.05)
        crash_db = crash / "native-queue.sqlite"
        assert rows(crash_db)[0]["state"] == "paused"
        assert "active owner" in run(native, "owner", crash, source, pack, runtime, marker, code=2).stderr
        alias = root / "alias"
        alias.mkdir()
        os.link(crash_db, alias / "native-queue.sqlite")
        assert "active owner" in run(native, "owner", alias, source, pack, runtime, marker, code=2).stderr
        holder.kill()
        holder.communicate(timeout=10)
        stale = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in crash.rglob("*.partial")}
        assert stale, "Crash did not exercise an active temporary writer"
        committed_segments = [p for p in crash.rglob("segment-*.mkv")]
        assert committed_segments, "Crash fixture did not commit a verified segment"
        orphan = committed_segments[0].parent / "segment-000001-0000000000000001.mkv"
        orphan.write_bytes(b"uncommitted checkpoint segment from a simulated crash window")
        unrelated = committed_segments[0].parent / "keep-note.txt"
        unrelated.write_text("preserve unrelated task data", encoding="utf-8")
        run(native, "recover", crash, source, pack, runtime, marker)
        recovered = rows(crash_db)
        assert [r["state"] for r in recovered] == ["completed"]
        assert recovered[0]["identity"]["source"]["sha256"] == crash_source_hash
        assert all(not p.exists() for p in stale), "Checkpoint resume retained stale task-owned partial writers"
        assert not orphan.exists(), "Checkpoint resume retained an unjournaled final segment"
        assert unrelated.read_text(encoding="utf-8") == "preserve unrelated task data"
        assert (crash / "crash-output.mkv").is_file()
        resumed = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries", "stream=nb_read_frames", "-of", "json", crash / "crash-output.mkv").stdout)
        assert resumed["streams"][0]["nb_read_frames"] == "66"
        assert pcm(crash_source) == pcm(crash / "crash-output.mkv")
        assert hashlib.sha256(crash_source.read_bytes()).hexdigest() == crash_source_hash
    finally:
        if holder.poll() is None:
            holder.kill()
            holder.communicate(timeout=10)
        holder_stdout.close()
        holder_stderr.close()

    # Also interrupt after every output frame has been committed but before
    # final assembly. Resume must assemble the saved segments with no pending
    # frame in memory and must not append a duplicate or invent a frame.
    complete = root / "crash-complete"
    complete.mkdir()
    complete_marker = complete / "paused.txt"
    complete_stdout = (complete / "holder.stdout.log").open("wb")
    complete_stderr = (complete / "holder.stderr.log").open("wb")
    complete_holder = subprocess.Popen([str(native), "hold-complete", str(complete), str(crash_source), str(pack), str(runtime), str(complete_marker)], stdout=complete_stdout, stderr=complete_stderr)
    try:
        deadline = time.monotonic() + 120
        while not complete_marker.exists():
            progress = (complete / "hold-progress.txt").read_text(encoding="utf-8", errors="replace") if (complete / "hold-progress.txt").exists() else "no processing event"
            assert complete_holder.poll() is None, ("last output frame: " + progress, (complete / "holder.stderr.log").read_text(encoding="utf-8", errors="replace"))
            assert time.monotonic() < deadline, "Complete-checkpoint worker failed to pause before final assembly"
            time.sleep(0.05)
        complete_db = complete / "native-queue.sqlite"
        assert rows(complete_db)[0]["state"] == "paused"
        complete_holder.kill()
        complete_holder.communicate(timeout=10)
        complete_segments = [p for p in complete.rglob("segment-*.mkv")]
        assert len(complete_segments) == 4, f"Expected all 64 output frames in four segments; found {len(complete_segments)}"
        run(native, "recover", complete, crash_source, pack, runtime, complete_marker)
        completed = rows(complete_db)
        assert len(completed) == 1 and completed[0]["state"] == "completed"
        complete_output = complete / "crash-output.mkv"
        complete_probe = json.loads(run(ffprobe, "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries", "stream=nb_read_frames", "-of", "json", complete_output).stdout)
        assert complete_probe["streams"][0]["nb_read_frames"] == "64"
        assert pcm(crash_source) == pcm(complete_output)
    finally:
        if complete_holder.poll() is None:
            complete_holder.kill()
            complete_holder.communicate(timeout=10)
        complete_stdout.close()
        complete_stderr.close()
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original
    report = json.loads((reports / "queue-workflow.json").read_text())
    report.update(cliBatch=True, sourceChangeRejected=True, processCrashDetected=True, hardLinkOwnerProtected=True,
                  restartFromZero=True, checkpointResume=True, stalePartialCleanup=True,
                  uncommittedSegmentCleanup=True, unrelatedTaskFilesPreserved=True, sequentialSessions=20,
                  repeatedPixelsAndTimingExact=True)
    (reports / "queue-workflow.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("Native queue, CLI batch, ownership, pause, cancellation, retry, source integrity and process-crash tests passed")

# Persistent local processing queue

The application layer and Qt desktop now expose an ordered SQLite-backed processing queue. It persists the complete versioned job configuration, verified source/model/device identity, progress and terminal result. It runs real RIFE jobs sequentially, isolates individual failures, and supports cooperative pause/resume/cancel, failed retries and explicit restart from frame zero. Desktop import accepts multiple files or folders, optional recursion, duplicate-source elimination, model color screening and collision-safe new outputs. Unsupported or unreadable inputs are reported rather than relabeled. The desktop and CLI use the same application import rules; the Studio remains available for a one-off job.

The desktop database defaults to `QStandardPaths::AppLocalDataLocation/queue.sqlite`; `--data-dir PATH` selects an isolated/local data directory. The adjacent `preferences.ini` stores only last-used local paths and encoding/FPS defaults. It does not store video contents or download models. Queue smoke tests use their own build-local data directory.

## Local CLI

```powershell
.\build\windows-dev\rifelove-cli.exe queue-add work/jobs.sqlite input.mkv output.mkv `
  --fps 60000/1001 `
  --model-dir models/cache/rife-4.25-fp32-sdr8-v2 `
  --runtime-dir C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0/lib `
  --encoder auto

.\build\windows-dev\rifelove-cli.exe queue-list work/jobs.sqlite
.\build\windows-dev\rifelove-cli.exe queue-run work/jobs.sqlite --progress
```

Repeat `queue-add` to append jobs. Its encoding options include `--pixel-format`, `--bitrate`, `--bframes`, repeated `--option KEY=VALUE`, `--gpu`, `--stream` and `--cut-threshold`. These are validated and persisted, not reconstructed from whichever settings happen to be selected later. Each model's descriptor ID/version/SHA-256 is pinned when queued; graph bytes, license metadata and tensor contract are fully verified by the executor.

`queue-list` opens an existing database read-only, including while a worker is active. `queue-export DATABASE` emits a version-1 JSON plan with a `jobs` array of `source`, `output` and `configuration`; `queue-import DATABASE PLAN_JSON` validates the whole plan before transactional insertion. Use new output names when importing an exported completed job. Import is limited to 10,000 jobs and 16 MiB; each versioned configuration is limited to 64 KiB. Duplicate pending/batch output paths are rejected with Windows case-insensitive comparison.

`queue-retry DATABASE ID` requeues a failed task, preserving its pinned identity and clearing previous progress/result atomically. `queue-cancel DATABASE ID` cancels queued/interrupted work when the runner is stopped. An active CLI runner handles Ctrl+C/Ctrl+Break by cancelling the active job and preserving later queued jobs. Cross-process pause/cancel IPC is not implemented; a desktop/service owner controls its own active queue directly.

Exit codes for `queue-run` are 0 for a drained queue without unresolved failures/interruption, 2 for failure, 3 for unresolved interrupted work, and 130 for console cancellation. Progress and terminal results are JSON Lines. Summaries describe the persisted queue, including historical failed/interrupted entries, rather than silently declaring unresolved work successful.

## Ownership, integrity and pause

An exclusive live Windows byte-range lock on the database file establishes ownership before recovery. The range is outside SQLite's own locking bytes and does not extend or modify the file. The lock follows the file identity across hard-link aliases and releases on process termination. A marker filename or stale PID alone never establishes ownership. Another queue owner fails before it can recover or mutate active states; read-only inspection remains available. Maintenance tools must still be used only after workers stop.

The source is opened under a native read lease that denies writes, deletion and replacement throughout hashing/inference/validation/publication. SHA-256 is streamed with a 1 MiB buffer and cancellation checks. Once preparation succeeds, actual source identity, verified model ID/version/hash and GPU/driver identity are stored before model inference. Retried/restarted tasks compare their saved source/model hashes and reject changed content. A source may still be edited while merely queued and before its first successful preparation; initial preparation defines its source-content snapshot.

The process currently admits one active native processing executor at a time, including independent queue instances and Studio operations. Waiting can be cancelled. Inference sessions and frame buffers are task-local; the reviewed ONNX/CUDA runtime set and root ONNX environment are process-scoped. Changing runtime DLL directories requires restarting the application. This policy prevents per-task runtime/provider unload/reload and avoids unqualified concurrent GPU sessions.

Pause is acknowledged at cooperative source-check/model/pipeline boundaries, with the paused state committed before its event is delivered. Resume continues the same live worker and open writer. Cancellation wakes a paused/waiting worker and removes cooperative partial outputs. Wall-clock throughput includes source verification, initialization, processing, validation, teardown and any user pause/wait time; it is separate from output FPS.

## Crash semantics

After obtaining ownership, recovery marks configured `running` and `paused` tasks `interrupted`. It leaves legacy task records and queued/terminal work unchanged. It does not start queued work automatically.

```powershell
.\build\windows-dev\rifelove-cli.exe queue-restart work/jobs.sqlite INTERRUPTED_ID new-output.mkv
.\build\windows-dev\rifelove-cli.exe queue-run work/jobs.sqlite --progress
```

Restart creates a separate task from frame zero under a new output name and preserves the original interrupted record. It is not segment checkpoint recovery. A crash may leave a temporary file, and a crash between final output publication and the SQLite terminal commit may leave a valid output associated with an interrupted task. Existing files are preserved; no published or orphaned output is deleted to make a retry pass. Owned-artifact tracking, safe stale-partial cleanup, segment checkpoints and stitching remain required.

## Evidence and remaining scope

`Queue.Integration` generates local fixtures and runs actual GPU/native jobs. It covers ordered batch completion, isolated failure and retry, queued/active/waiting cancellation, pause/resume, source write protection and changed-source rejection, atomic import, export, durable identities/results, second-owner/hard-link protection, and termination of a writer after nonempty Matroska data reached disk. Reopening detects interruption; restarting produces 360 correctly timed frames with bit-identical decoded audio and unchanged source content. Old temporary bytes remain untouched.

A repeated-session regression executes 20 separate native inference sessions in one process and independently checks identical decoded pixels/timing. This was added after a recorded intermittent `cudnn_heuristic64_9.dll` access violation during repeated session/runtime creation; the shared runtime/environment lifetime policy is exercised by the regression. It is a bounded local stability result, not a multi-hour or supported-hardware claim. Queue and repeated-session evidence is written to local `queue-workflow.json` / `queue-stress.json` reports.

The actual QML desktop workflow test covers recursive folder import, duplicate selections, same-basename collision-safe outputs, visible HDR rejection, sequential native processing, UI pause/resume, a deterministic output-collision failure and controller retry, queued-task cancellation and preference reload. Independent FFmpeg/ffprobe checks inspect all completed queue outputs, frame counts, audio samples and source integrity. The remaining work includes audit/history pagination, cross-process live control, safe artifact/checkpoint ownership, true segment resume/stitching, disk-full/OOM/fault injection and multi-hour/hardware/content qualification. The complete Master Plan remains the acceptance baseline.

# Persistent local processing queue

The application layer and Qt desktop expose an ordered SQLite-backed processing queue. It persists the complete versioned job configuration, verified source/model/device identity, progress and terminal result. It runs real RIFE jobs sequentially, isolates individual failures, and supports cooperative pause/resume/cancel, failed retries, restart from frame zero, and explicit resume from verified video segments after a process interruption. Desktop import accepts multiple files or folders, optional recursion, duplicate-source elimination, model color screening and collision-safe new outputs. Unsupported or unreadable inputs are reported rather than relabeled. The desktop and CLI use the same application import rules; the Studio remains available for a one-off job.

The desktop database defaults to `QStandardPaths::AppLocalDataLocation/queue.sqlite`; `--data-dir PATH` selects an isolated/local data directory. The adjacent `preferences.ini` stores only last-used local paths and encoding/FPS defaults. It does not store video contents or download models. Queue smoke tests use their own build-local data directory.

## Local CLI

```powershell
.\build\windows-dev\rifelove-cli.exe queue-add work/jobs.sqlite input.mkv output.mkv `
  --fps 60000/1001 `
  --model-dir models/cache/rife-4.25-fp32-sdr8-v3 `
  --runtime-dir C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0/lib `
  --encoder auto

.\build\windows-dev\rifelove-cli.exe queue-list work/jobs.sqlite
.\build\windows-dev\rifelove-cli.exe queue-run work/jobs.sqlite --progress
```

For an interrupted task with a verified checkpoint, use `queue-resume DATABASE INTERRUPTED_ID` and then run `queue-run` again. This preserves the existing task and output path. In the desktop queue, select **从检查点继续** for an interrupted task. **从头重启** creates a separate task and new output from frame zero.

Repeat `queue-add` to append jobs. Its options include `--restoration-model-dir` for a supported local restoration pack, `--pixel-format`, `--bitrate`, `--bframes`, repeated `--option KEY=VALUE`, `--gpu`, `--stream` and `--cut-threshold`. These are validated and persisted, not reconstructed from whichever settings happen to be selected later. Each model's descriptor ID/version/SHA-256 is pinned when queued; graph bytes, license metadata and tensor contract are fully verified by the executor. Restoration checkpoints also record the selected backend and runtime/tile signature. The worker compares these after loading the model and refuses to reuse segments when they differ.

`queue-list` opens an existing database read-only, including while a worker is active. `queue-report DATABASE [--output JSON]` creates a diagnostic snapshot in the CLI or the desktop Queue page. It includes legacy/configured tasks, states, failure reasons, stored identity/configuration/results, completed output/AI-frame totals, aggregate completed output FPS, and scene/duplicate/abnormal-motion counts. Reports are read-only and refuse to replace an existing file; perceptual metrics are explicitly marked unmeasured. `queue-export DATABASE` emits a version-1 JSON plan with a `jobs` array of `source`, `output` and `configuration`; `queue-import DATABASE PLAN_JSON` validates the whole plan before transactional insertion. Use new output names when importing an exported completed job. Import is limited to 10,000 jobs and 16 MiB; each versioned configuration is limited to 64 KiB. Duplicate pending/batch output paths are rejected with Windows case-insensitive comparison.

`queue-retry DATABASE ID` requeues a failed task, preserving its pinned identity and clearing previous progress/result atomically. `queue-cancel DATABASE ID` cancels queued/interrupted work when the runner is stopped. An active CLI runner handles Ctrl+C/Ctrl+Break by cancelling the active job and preserving later queued jobs. Cross-process pause/cancel IPC is not implemented; a desktop/service owner controls its own active queue directly.

Exit codes for `queue-run` are 0 for a drained queue without unresolved failures/interruption, 2 for failure, 3 for unresolved interrupted work, and 130 for console cancellation. Progress and terminal results are JSON Lines. Summaries describe the persisted queue, including historical failed/interrupted entries, rather than silently declaring unresolved work successful.

## Ownership, integrity and pause

An exclusive live Windows byte-range lock on the database file establishes ownership before recovery. The range is outside SQLite's own locking bytes and does not extend or modify the file. The lock follows the file identity across hard-link aliases and releases on process termination. A marker filename or stale PID alone never establishes ownership. Another queue owner fails before it can recover or mutate active states; read-only inspection remains available. Maintenance tools must still be used only after workers stop.

The source is opened under a native read lease that denies writes, deletion and replacement throughout hashing/inference/validation/publication. SHA-256 is streamed with a 1 MiB buffer and cancellation checks. Once preparation succeeds, actual source identity, verified model ID/version/hash and GPU/driver identity are stored before model inference. Retried/restarted tasks compare their saved source/model hashes and reject changed content. A source may still be edited while merely queued and before its first successful preparation; initial preparation defines its source-content snapshot.

The process currently admits one active native processing executor at a time, including independent queue instances and Studio operations. Waiting can be cancelled. Inference sessions and frame buffers are task-local; the reviewed ONNX/CUDA runtime set and root ONNX environment are process-scoped. Changing runtime DLL directories requires restarting the application. This policy prevents per-task runtime/provider unload/reload and avoids unqualified concurrent GPU sessions.

Pause is acknowledged at cooperative source-check/model/pipeline boundaries, with the paused state committed before its event is delivered. Resume continues the same live worker and open writer. Cancellation wakes a paused/waiting worker and removes cooperative partial outputs. Wall-clock throughput includes source verification, initialization, processing, validation, teardown and any user pause/wait time; it is separate from output FPS.

## Crash semantics

After obtaining ownership, recovery marks configured `running` and `paused` tasks `interrupted`. Before presenting the queue, startup checks any interrupted task with a saved pre-publication proof and an existing final output. It holds an exclusive read lease while hashing the file, validates the saved result schema/frame counts, then atomically adopts the output as completed only when the size and SHA-256 match. A missing output remains eligible for checkpoint resume. A modified/unreadable file or invalid proof stays interrupted with a diagnostic; the file is left untouched. Recovery does not start queued work automatically.

An interrupted task can use either of these paths:

```powershell
.\build\windows-dev\rifelove-cli.exe queue-resume work/jobs.sqlite INTERRUPTED_ID
.\build\windows-dev\rifelove-cli.exe queue-run work/jobs.sqlite --progress

# Or create a separate task that starts at frame zero:
.\build\windows-dev\rifelove-cli.exe queue-restart work/jobs.sqlite INTERRUPTED_ID new-output.mkv
```

Checkpoint resume is allowed only when the saved manifest matches the current local job, source/model identity and media runtime, the requested output does not already exist, and every committed segment passes its stored hash and full media/timeline verification. The production queue commits a video-only segment every 256 output frames. It decodes from the source beginning to rebuild RIFE context, skips inference for the committed output prefix, generates the rest, then validates and stream-copies the segments into the final file with source audio and supported auxiliary streams. The desktop offers **从检查点继续** for interrupted tasks and keeps **从头重启** as a separate action.

### Media and processing-pipeline content identity

New manifests retain the database's schema-2 envelope and add a version-2
`mediaPipelineIdentity`. The human-readable FFmpeg version remains diagnostic
only. The identity binds the actual loaded `avcodec`, `avformat`, `avutil` and
`swscale` DLLs by SHA-256 and byte count, plus each library's compiled
configuration digest and ABI version. It follows resolved import-address-table
targets to fingerprint linked non-OS dependencies, including external codec
libraries such as OpenH264, Kvazaar and AOM, and `swresample` if imported. This
also covers software fallbacks of an `auto` encoder. Ordinary Windows system
libraries are excluded; known third-party codec/library names are still included
if installed below the Windows directory. Dynamic GPU driver libraries are
governed by the existing GPU/runtime identity and are not claimed to be covered
by this FFmpeg import closure.

Core modules are located using their returned compiled-configuration data
addresses, avoiding executable import stubs that could identify the EXE instead
of a MinGW DLL. Dependencies are resolved from loaded IAT addresses rather than
guessed PATH locations. Actual file contents are read under write/delete-denying
read leases with a bounded 1 MiB hashing buffer. Installation directories,
timestamps and file IDs are not identity inputs; identical DLL bytes at a new
location remain equivalent. Hashing is cancellation-aware and performed when
creating/checking the manifest; it is not a per-frame operation. A failed lookup
or read fails verification instead of falling back to a version string.

Writer identity includes the explicit conversion algorithm
`swscale-bicubic-layout-lanczos-resize-accurate-rnd-matrix-range-v2` and a build-time
SHA-256 of `src/media/video_writer.cpp`. CMake watches that source and regenerates
the compiled digest when it changes. Consequently, the corrected chroma-layout
filter cannot reuse segments whose old writer implementation is unproven, even
when both FFmpeg builds report 8.1.3. Future conversion changes must keep the
algorithm label accurate; the source digest provides an additional automatic
guard.

The embedded `processingPipeline` proof has its own schema version 1 and
`rife-processing-source-contract-v1` algorithm contract. Its explicit reviewed
file inventory is maintained in `cmake/ProcessingPipelineIdentity.cmake`:
all production `src/` files and headers, the root build definition, this identity
module and generated-header templates. It therefore covers RIFE color,
decode/encode, timeline scheduling, interpolation, scene detection, restoration,
GPU/inference and checkpoint replay changes, including header-only changes.
Tests, native/QML UI files, packaging and model weights are outside that list;
model and runtime identity checks still apply independently. A configure-time
omission audit rejects any new unreviewed file under `src/`. All controlled
files are configure dependencies, so their changes regenerate the compiled proof.

The build emits an auditable `processing_pipeline_identity.json` alongside its
generated header. Its aggregate SHA-256 covers `algorithm=<version>\n` followed
by sorted `<relative/path>=<file SHA-256>\n` records. The proof also binds the
compiler ID/version, explicit single-configuration build type, C++20 contract
and SHA-256 of global/configuration C++ flags. Manifest loading independently
recomputes the aggregate and then requires the entire processing proof to match.
This is deliberately conservative: even a non-pixel production-source change
can require a fresh output job. Build paths and installed source files are not
runtime identity inputs. No source tree is required on the user's machine.

**Unfinished legacy tasks:** a manifest carrying only a version string, or the
earlier version-1 DLL/writer proof without processing-source identity, has no
complete proof of which pipeline produced its segments. Explicit resume rejects
it with a diagnostic requiring a new output job. The database/task configuration,
identity, manifest, committed segment bytes and even uncommitted segment files
are preserved. No cleanup occurs before media identity matches. This release
does not infer a safe exception merely from a source pixel format: the saved
proof does not establish every writer operation that happened. A settings-only
legacy schema migration is still testable as a historical transformation, but
does not bypass the new queue's mandatory media proof.

**Already completed/published outputs:** existing results are not concatenated
with newly encoded segments. Completed history is retained. Startup reconciliation
of an already published output continues to require its saved full-file hash,
byte count and validation-result proof; this transition does not re-encode or
delete that output and does not certify an old result's perceptual color quality.
The new media identity gates segment reuse, rather than reclassifying all old
completed files as current-writer output.

CPU regressions compare each recorded DLL digest with an independent loaded
module address/file hash, reject each changed core/external DLL and writer
identity despite equal version strings, and exercise real SQLite checkpoint
creation and same-proof resume. A separate four-case legacy-publication
regression preserves completed history, adopts a byte-identical already
published FFV1 file only with saved validation proof, and refuses changed or
unproven final files without touching them. Changed/missing media proof is rejected while
configuration, identity, manifest and committed/uncommitted file bytes remain
intact. These fixtures run FFV1 media verification without GPU/model inference;
they are not a multi-hour or all-codec recovery qualification.

Source-identity regressions additionally change RIFE color, timeline and
restoration source/header digests while retaining the writer digest, reseal the
aggregate, and require refusal against the current build. They cover algorithm,
compiler/build flags, malformed aggregate and old proof refusal; the same proof
still resumes. A pure CMake fixture checks deterministic generation, actual
source/header edits, compiler flags and the unreviewed-source omission guard.

Historical embedded-media-schema-1 evidence, executed 2026-10-31: independent
CPU probes loaded the baseline and x86asm SDKs
in separate processes and checked every core digest against that SDK's actual
file bytes. Both reported FFmpeg `8.1.3`; their `avcodec` SHA-256 values were
`d33ecd64d8f526e04b642889892d27e8b56f45f0c1e43d90e42c9afa443e6844`
and `d9ecfb2355486d3870495444340047c2e883a3837e6f53b0a93dd354785ece9a`
respectively. The `swscale` digests also differed:
`610e15224bc94f8610ede5a0227247ff76810523cd97ad02458f057af9f2af5d`
(baseline) and `f2745605d914c8ba6828853b6bad2fe4581eca68c5665512014f338ec7afc137`
(x86asm). Both identities captured seven linked dependencies: AOM, Kvazaar,
OpenH264, swresample, libgcc, libstdc++ and libwinpthread. The writer source
identity was the same. Total file bytes hashed per identity were 41,037,782
(baseline) and 43,166,166 (x86asm), using the bounded buffer above; this is not
per-frame work. The ignored evidence is
`work/verification/media-identity-20261031/{baseline.xml,x86asm.xml,summary.json}`.
The probe executable there is a hard link to the local test executable, not an
additional distributed application or a model bundle. The selected CPU suite
covering queue/media identity, stored tasks, settings, batch policy and dynamic
noise controls passed 49/49, including the four-case legacy-publication fixture.
No GPU processing was run for this identity check.

A separate historical media-schema-1 GPU queue replay rebuilt the independent x86asm driver
and ran the existing 12-source-frame RIFE + automatic FastDVDnet synthetic
fixture through ONNX Runtime CUDA. Fresh tasks wrote identical media proof;
an intentionally terminated paused worker recovered in a new process, retained
the original serialized manifest and reused two committed segments unchanged
(4 frames), processing the remaining 20. The complete 24-frame decoded pixels,
PTS and decoded audio matched the uninterrupted reference, and the source hash
was unchanged. Core DLL digests were checked against the selected x86asm SDK,
and the compiled writer digest matched its source. Evidence is retained under
`work/verification/automatic-denoise-checkpoint-media-identity-x86asm-20261031/`.
This proves that earlier small same-identity GPU recovery path, not the newer
source-proof schema, cross-build reuse or multi-hour qualification. The later
schema-2 run below uses separately rebuilt binaries.

On 2026-10-31 a rebuilt source-proof-schema-2 driver repeated the same small
synthetic RIFE plus FastDVDnet CUDA interruption test. It reused four frames in
two committed segments; the 24 decoded output frames, their PTS and PCM audio
matched uninterrupted processing, and the source hash was unchanged. Evidence
is retained locally in
`work/verification/automatic-denoise-checkpoint-r13-20261031/summary.json`.
This is same-build, same-runtime, short-clip evidence, not cross-build or
multi-hour qualification.

Completion persistence also has seven SQLite fault-injection cases in
`tests/integration/queue_finalization_test.cpp`. A CPU fixture drives the real
checkpoint journal, validation, publication proof and no-replace output. If
the completion/result write or the final progress callback fails after
publication, the worker leaves the task interrupted and retains the proof and
file; recovery verifies their size, hash and result before adoption. Failures
between publication and its final event are treated as uncertain when a proof
and destination both exist. A competing or modified file is never adopted
without matching proof. These tests replace GPU inference and do not cover all
filesystem, process-crash or database failure timings.

Committed segments and uncommitted `.partial` files are left untouched when the process first reopens the database. On an explicit checkpoint-resume request, RifeLove first verifies the immutable task manifest and every committed segment, then removes only unjournaled `segment-*.mkv` files and matching `.partial` writers at or beyond the committed segment prefix inside that task's checkpoint directory. It preserves unrelated files and never scans or deletes partial outputs beside the requested final output. Recovery never treats a partial file as a complete checkpoint. A previously validated final output is adopted automatically after restart only when its saved byte count, SHA-256 and validation-result structure all match. A modified output or damaged proof stays interrupted and is never overwritten. Cooperative cancellation removes its own live partial output; process termination does not.

## Evidence and remaining scope

`Queue.Integration` generates local fixtures and runs actual GPU/native jobs. It covers ordered batch completion, isolated failure and retry, queued/active/waiting cancellation, pause/resume, source write protection and changed-source rejection, atomic import/export, durable identities/results, second-owner/hard-link protection, and process termination with one verified checkpoint plus a live partial segment. Reopening detects interruption; checkpoint resume validates/reuses the committed segment, removes the stale partial and a simulated unjournaled segment while preserving an unrelated note, then produces the complete 66-frame output with bit-identical decoded audio and unchanged source content. A second crash occurs after all 64 expected frames have been committed but before final assembly; resume assembles the saved segments without an in-memory pending frame and produces exactly 64 frames with bit-identical audio. A third crash occurs after final output publication but before the SQLite completion commit; startup automatically adopts the unchanged file, while altered bytes and a corrupted result proof remain interrupted without changing the output. These tests use 16-frame segments to reach crash points quickly; the application default is 256 frames. This is bounded local evidence, not a multi-hour or broad hardware/content qualification.

A repeated-session regression executes 20 separate native inference sessions in one process and independently checks identical decoded pixels/timing. This was added after a recorded intermittent `cudnn_heuristic64_9.dll` access violation during repeated session/runtime creation; the shared runtime/environment lifetime policy is exercised by the regression. It is a bounded local stability result, not a multi-hour or supported-hardware claim. Queue and repeated-session evidence is written to local `queue-workflow.json` / `queue-stress.json` reports.

The actual QML desktop workflow test covers recursive folder import, duplicate selections, same-basename collision-safe outputs, visible HDR rejection, sequential native processing, UI pause/resume, a deterministic output-collision failure and controller retry, queued-task cancellation and preference reload. Independent FFmpeg/ffprobe checks inspect completed queue outputs, frame counts, audio samples and source integrity. Remaining work includes audit/history pagination, cross-process live control, orphan cleanup/reconciliation, fault injection at all crash boundaries, disk-full/OOM coverage and multi-hour/hardware/content qualification. The complete Master Plan remains the acceptance baseline.

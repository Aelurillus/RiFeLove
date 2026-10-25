# RifeLove

RifeLove is an offline Windows video frame interpolation application in active development. Its complete product scope is recorded in [the Master Plan](docs/MASTER_PLAN.md). Native RIFE video processing now runs locally; the full desktop product and release acceptance remain in development.

## Current state

- Exact rational time arithmetic and a VFR-to-CFR output timeline planner.
- A persistent SQLite task store with validated state transitions and interrupted-task recovery.
- A JSON-lines CLI for planning an output timeline and inspecting/recovering tasks.
- A native FFmpeg media engine for local inspection, original-precision video decode and MP4/MKV/MOV stream-copy remuxing, with audio/subtitle/chapter retention and no-overwrite outputs.
- A model-independent generated-frame encoder/mux writer and native transcode path, with exact timestamps, bounded encoder lookahead tracking, auxiliary stream copy and JSON progress.
- A native CUDA driver layer, tested on the local GTX 1660 SUPER for device queries, owned memory, bounded allocations and transfers.
- Checksum-pinned RIFE 4.25/4.26 export/reference tools and native ONNX Runtime CUDA/TensorRT backends; both models pass small-profile SDR8 numerical comparisons and actual GPU operator-placement checks locally.
- A streaming native RIFE pipeline with rational CFR scheduling, conservative cut/duplicate protection, actual encoder-open fallback and decoded-output validation before publication.
- A persistent native/CLI batch queue with pinned configurations, source/model identities, pause/cancel/retry APIs, live ownership and interruption detection. [Queue contracts and limits](docs/development/QUEUE.md).
- A connected Chinese Qt/QML Studio, SQLite task queue and offline Model Center. The desktop supports single-video processing, recursive batch/folder import, local preferences, queue progress, pause/resume, retry/cancellation, and persisted model selection. Model-pack schema v2 verifies weight, license, source-manifest and validation-report hashes, checks the declared capabilities and validation results, and shows the model's qualification boundaries; legacy v1 packs remain readable.
- A local desktop-and-CLI bundle with RIFE 4.25 selected by default and RIFE 4.26 available in the offline Model Center. Build it with `packaging/windows/package-local.ps1`; output is written to the ignored `out/RifeLove-local/` directory and starts by double-clicking `RifeLove.exe`. The bundle links this workstation's ONNX Runtime/TensorRT/CUDA/cuDNN files, so it is not a transferable installer.
- CMake/Ninja builds and automated tests. Qt 6.12 builds locally with a matching MinGW 15.1 toolchain; [desktop setup and checks](docs/development/DESKTOP.md).

Development currently stays on the local machine: local Git commits, local builds, local test reports and local task databases. Remote repositories, hosted CI and release publishing are deferred. A future Windows CI template is kept under `docs/development/templates/` and is not active.

The current AI video adapter supports explicitly tagged BT.709 and BT.470BG/SMPTE 170M 8/10-bit SDR, using the tagged YUV matrix and preserving the source profile and pixel format when the selected encoder supports it. A narrowly verified H.264 VUI/container disagreement within the BT.601-family SDR tags is normalized to the container metadata when layout, bit depth, primaries and range match. The local run folder includes two checksum-verified RIFE packs and tested defaults; quality-ranked profiles and real-content comparison remain required. Studio and Queue expose H.264 NVENC VBR + CQ for systems whose source-profile preflight succeeds; this workstation's driver cannot initialize NVENC, so successful hardware CQ output is not yet verified. In automatic mode, that failure now falls back to a supported software encoder while RIFE inference stays on CUDA. Small synthetic MP4/MKV/MOV, H.264/HEVC/AV1, VFR and audio/subtitle cases pass, including an independent PyTorch video comparison. TensorRT FP32 is integrated and passes small-profile native checks; a synthetic 10-bit RIFE pipeline case now verifies AV1 fallback and bit-depth/tag preservation. A bounded native crash/recovery test now verifies one committed segment, resumes without recomputing its output frames, and preserves frame count and PCM audio. Real-content 10-bit quality, full-resolution/content qualification, FP16, HDR processing, multi-hour checkpoint coverage, broader hardware qualification, quality-ranked model profiles and an audited installer remain required. See the [pipeline evidence and limits](docs/development/PIPELINE.md), [desktop queue guide](docs/development/QUEUE.md), [SRS](docs/SRS.md) and [architecture](docs/architecture/ADD.md).

## Build

Use CMake 3.25+, Ninja and a C++20 compiler. The local development presets select MinGW. Configure uses local dependency caches by default; it fails early if a required cache is missing. Run the local check from PowerShell:

```powershell
.\tools\development\check.ps1
.\tools\development\check.ps1 -Preset windows-release
```

This configures, builds and tests locally, writing `local-check.json` and `test-report.xml` into the selected build directory. The core and CLI build without Qt or NVIDIA SDKs. Python is used only for development CLI integration checks; the application runtime has no Python dependency. MSVC and the Qt/NVIDIA release configuration still require validation.

If a new machine has no local dependency sources, they can be supplied through CMake's `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` settings. Initial online setup is an explicit opt-in using `check.ps1 -AllowDependencyDownload`; it fetches pinned sources and does not publish project data. See [local development](docs/development/LOCAL.md).

Enable the media engine by supplying a local FFmpeg SDK with `check.ps1 -FFmpegRoot '<SDK root>'`. A checksum-pinned SDK setup helper and tested limitations are documented in [FFmpeg development](docs/development/FFMPEG.md). Normal builds do not acquire this SDK automatically.

## CLI

`rifelove-cli timeline --fps 60/1 --end 2/1 --pts 0/1,1/24,1/12,1/8` emits one JSON object per output frame. The PTS values are seconds relative to the clip origin; `--end` is the exclusive clip end. This command plans timestamps and source-frame pairs; it does not decode or interpolate pixels.

`rifelove-cli tasks <database-path>` opens an existing task database read-only and lists tasks without changing their state. `rifelove-cli task-create <database-path> <source> <output>` persists a new queued task. Paths, including Chinese names and spaces, are accepted as Unicode on Windows and JSON output is UTF-8.

`rifelove-cli tasks-recover <database-path>` is a separate legacy maintenance operation for an existing database, to be used after all workers have stopped. It marks `running`/`paused` records as `interrupted`; configured queue tasks can then use `queue-resume DATABASE INTERRUPTED_ID` to resume from verified segments, or `queue-restart DATABASE INTERRUPTED_ID NEW_OUTPUT` to start over. The process decodes the source from the beginning to rebuild RIFE context, skips already completed output frames, and finalizes only after segment and full-output validation.

With the media engine enabled, `media-probe INPUT` reports streams/metadata, `media-scan INPUT --frames` decodes and reports rational PTS/color plus any evidence-based PTS repairs, and `remux INPUT OUTPUT` copies compatible streams into a new MP4/MOV/MKV file. Remuxing preserves compressed streams; it does not change frame rate or generate AI frames.

`transcode INPUT OUTPUT --encoder NAME --progress` re-encodes the selected video at its original presentation times while copying other compatible streams. Software H.264/HEVC/AV1 and FFV1 are covered by the local suite. The current FFmpeg SDK's NVENC path is incompatible with this machine's driver; explicit encoder requests fail visibly. See [encoding contracts and tests](docs/development/ENCODING.md).

`interpolate INPUT OUTPUT --fps 60000/1001 --model-dir PACK --runtime-dir DLL_DIRECTORY --progress --report report.json` generates actual RIFE frames and validates the finalized video before publishing a new file. Its `auto` encoder tries NVENC and then selects a source-compatible software encoder; this 8-bit workstation uses software H.264, while the synthetic 10-bit SDR test selects AV1 to preserve bit depth. The optional no-overwrite report records model/GPU identity, actual encoder and fallback, end-to-end throughput, per-stage wall times, quality-protection counters and timing validation; it explicitly does not claim perceptual scores. The desktop Studio also shows cumulative inference, encoding, decode, color/preparation and validation time while processing. Model/runtime paths are explicit local directories; no model is downloaded by this command. [Pipeline guide](docs/development/PIPELINE.md).

Use a versioned profile exported from Studio with `profile-check FILE.json`, then pass it to `interpolate` or `queue-add` using `--profile FILE.json`. Input/output paths remain explicit, and the output extension must match the profile container. Explicit CLI options after the profile override profile settings. [CLI profile guide](docs/CLI.md).

`gpu-info [DEVICE_INDEX]` reports the installed CUDA driver's actual device, compute capability and memory. This command does not claim that ONNX, TensorRT, NVDEC or NVENC is available.

`media-capabilities INPUT [--fps RATE]` opens encoder contexts for the source's dimensions/pixel format and reports whether H.264 NVENC, software H.264, HEVC and AV1 are registered and initialize. This is a preflight check, not proof that encoded frames were produced; the processing pipeline still validates the real output.

## Licensing

The application source is licensed under MIT. SQLite is public domain, and the pinned GoogleTest source uses BSD-3-Clause; both are fetched at pinned versions and are not copied into the repository. FFmpeg, Qt, NVIDIA libraries, RIFE code and weights require separate version/build-specific redistribution review before packaging. No model weights or vendor runtime binaries are included.

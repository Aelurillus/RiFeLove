# RifeLove

RifeLove is an offline Windows video frame interpolation application in active development. Its complete product scope is recorded in [the Master Plan](docs/MASTER_PLAN.md); this repository currently contains the first tested core foundations, not a finished video processor.

## Current state

- Exact rational time arithmetic and a VFR-to-CFR output timeline planner.
- A persistent SQLite task store with validated state transitions and interrupted-task recovery.
- A JSON-lines CLI for planning an output timeline and inspecting/recovering tasks.
- A native FFmpeg media engine for local inspection, original-precision video decode and MP4/MKV/MOV stream-copy remuxing, with audio/subtitle/chapter retention and no-overwrite outputs.
- A model-independent generated-frame encoder/mux writer and native transcode path, with exact timestamps, bounded encoder lookahead tracking, auxiliary stream copy and JSON progress.
- A native CUDA driver layer, tested on the local GTX 1660 SUPER for device queries, owned memory, bounded allocations and transfers.
- Checksum-pinned RIFE 4.25 export/reference tools and a compiled native ONNX Runtime CUDA backend; its GPU numerical validation is pending runtime setup.
- CMake/Ninja builds and automated unit tests. Qt desktop shell is enabled automatically when Qt 6 Quick is present.

Development currently stays on the local machine: local Git commits, local builds, local test reports and local task databases. Remote repositories, hosted CI and release publishing are deferred. A future Windows CI template is kept under `docs/development/templates/` and is not active.

End-to-end RIFE video processing, TensorRT, the desktop workflow, full color/quality processing and packaging remain roadmap work. The software does not yet generate AI-interpolated video. See the [SRS](docs/SRS.md), [architecture design](docs/architecture/ADD.md), [RIFE validation](docs/development/RIFE_MODELS.md) and [CUDA backend](docs/development/GPU.md) for current evidence and limits.

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

`rifelove-cli tasks-recover <database-path>` is a separate maintenance operation for an existing database, to be used after all workers have stopped. It marks `running` records as `interrupted`; it does not resume video processing. Automatic recovery with worker ownership and safe segment checkpoints remains roadmap work.

With the media engine enabled, `media-probe INPUT` reports streams/metadata, `media-scan INPUT --frames` decodes and reports rational PTS/color, and `remux INPUT OUTPUT` copies compatible streams into a new MP4/MOV/MKV file. Remuxing preserves compressed streams; it does not change frame rate or generate AI frames.

`transcode INPUT OUTPUT --encoder NAME --progress` re-encodes the selected video at its original presentation times while copying other compatible streams. Software H.264/HEVC/AV1 and FFV1 are covered by the local suite. The current FFmpeg SDK's NVENC path is incompatible with this machine's driver; no successful GPU encoding or automatic fallback is claimed. See [encoding contracts and tests](docs/development/ENCODING.md).

`gpu-info [DEVICE_INDEX]` reports the installed CUDA driver's actual device, compute capability and memory. This command does not claim that ONNX, TensorRT, NVDEC or NVENC is available.

## Licensing

The application source is licensed under MIT. SQLite is public domain, and GoogleTest is Apache-2.0; both are fetched at pinned versions and are not copied into the repository. FFmpeg, Qt, NVIDIA libraries, RIFE code and weights require separate version/build-specific redistribution review before packaging. No model weights or vendor runtime binaries are included.

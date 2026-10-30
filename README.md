# RifeLove

RifeLove is an offline Windows desktop application for video frame interpolation in active development. Its complete product scope is recorded in [the Master Plan](docs/MASTER_PLAN.md), with the user's later scope decision that the deliverable is the desktop application only. Native RIFE video processing now runs locally; the full desktop product and release acceptance remain in development. This checkout is not a formal stable release.

## Current state

- Exact rational time arithmetic and a VFR-to-CFR output timeline planner.
- A persistent SQLite task store with validated state transitions and interrupted-task recovery.
- A native FFmpeg media engine for local inspection, original-precision video decode and MP4/MKV/MOV stream-copy remuxing, with audio/subtitle/chapter retention and no-overwrite outputs.
- A model-independent generated-frame encoder/mux writer and native transcode path, with exact timestamps, bounded encoder lookahead tracking, auxiliary stream copy and JSON progress.
- A native CUDA driver layer, tested on the local GTX 1660 SUPER for device queries, owned memory, bounded allocations and transfers.
- Checksum-pinned RIFE 4.25 export/reference tooling and native ONNX Runtime CUDA/TensorRT backends. Historical 4.25 Lite, 4.26 and experimental 4.22 Lite packs were evaluated and removed from the active model library; only 4.25 has the recorded end-to-end 720p video comparison.
- A streaming native RIFE pipeline with rational CFR scheduling, conservative cut/duplicate protection, actual encoder-open fallback and decoded-output validation before publication.
- A persistent local desktop task queue with pinned configurations, source/model identities, pause/cancel/retry APIs, live ownership and interruption detection. [Queue contracts and limits](docs/development/QUEUE.md).
- A native Windows desktop entry point using Win32 controls, Direct2D and DirectWrite. The current local preview executable is `RifeLoveNative.exe`; it has no Qt, QML or CLI dependency. Queue history, comparison preview, frame stepping, model catalog management, batch-folder controls, pause/resume and checkpoint recovery are still being migrated into this native surface.
- A local native preview bundle built with `packaging/windows/package-native-local.ps1`; the default output is the ignored `out/RifeLoveNative-local-nvenc/` directory. It contains two selectable default model packs: RIFE 4.25 FP32 SDR8 v3 for interpolation and Real-ESRGAN x2plus SDR8 v2 for restoration. A legacy RIFE v2 pack is retained only when verified local migration data requires its exact identity. The bundle links this workstation's ONNX Runtime/TensorRT/CUDA/cuDNN files, so it is a local validation package rather than a transferable installer.
- CMake/Ninja builds and automated tests. Qt 6.12 setup remains documented for historical Studio work; the current native preview is built with the matching MinGW toolchain. See [desktop setup and checks](docs/development/DESKTOP.md).

Development currently stays on the local machine: local Git commits, local builds, local test reports and local task databases. Remote repositories, hosted CI and release publishing are deferred. A future Windows CI template is kept under `docs/development/templates/` and is not active. The older Qt/QML Studio remains in the tree as historical implementation work; it is not the current native preview delivery.

The current AI video adapter supports explicitly tagged BT.709 and BT.470BG/SMPTE 170M 8/10-bit SDR, using the tagged YUV matrix and preserving the source profile and pixel format when the selected encoder supports it. A narrowly verified H.264 VUI/container disagreement within the BT.601-family SDR tags is normalized to the container metadata when layout, bit depth, primaries and range match. The local native preview package contains the two default model packs described above; quality-ranked profiles and real-content comparison remain required. The isolated FFmpeg 8.1.3 SDK has been validated on the local GTX 1660 SUPER for H.264 NVENC, HEVC NVENC 8-bit/10-bit, CUDA/NVDEC decode, OpenH264, Kvazaar and libaom software paths, plus a native CUDA RIFE run. The packaged SDK still uses the no-x86asm build, so its CPU decode/software-encode throughput remains unqualified; a separate NASM-enabled x86asm rebuild passed real file, NVENC/NVDEC, native DLL, RIFE/CUDA and CPU A/B checks but remains a validation artifact until it is deliberately promoted. Automatic mode can fall back to a supported software encoder while RIFE inference stays on CUDA. Real-content 10-bit quality, full-resolution/content qualification, FP16, HDR processing, multi-hour checkpoint coverage, broader hardware qualification, quality-ranked model profiles, native queue/preview migration and an audited installer remain required. See the [pipeline evidence and limits](docs/development/PIPELINE.md), [desktop queue guide](docs/development/QUEUE.md), [SRS](docs/SRS.md) and [architecture](docs/architecture/ADD.md).

## Build

Use CMake 3.25+, Ninja and a C++20 compiler. The local development presets select MinGW. Configure uses local dependency caches by default; it fails early if a required cache is missing. Run the local check from PowerShell:

```powershell
.\tools\development\check.ps1
.\tools\development\check.ps1 -Preset windows-release
```

This configures, builds and tests locally, writing `local-check.json` and `test-report.xml` into the selected build directory. The native core and internal test utilities build without Qt or NVIDIA SDKs; these utilities are not part of the RifeLove user-facing deliverable. Python is used only by development checks; the desktop application runtime has no Python dependency. MSVC and the Qt/NVIDIA release configuration still require validation.

If a new machine has no local dependency sources, they can be supplied through CMake's `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` settings. Initial online setup is an explicit opt-in using `check.ps1 -AllowDependencyDownload`; it fetches pinned sources and does not publish project data. See [local development](docs/development/LOCAL.md).

Enable the media engine by supplying a local FFmpeg SDK with `check.ps1 -FFmpegRoot '<SDK root>'`. A checksum-pinned SDK setup helper and tested limitations are documented in [FFmpeg development](docs/development/FFMPEG.md). Normal builds do not acquire this SDK automatically.

The isolated source-build recipe for the NVIDIA-compatible SDK and its NASM/x86asm comparison is documented in [FFMPEG_SOURCE_BUILD.md](docs/development/FFMPEG_SOURCE_BUILD.md). It writes only under `work/` and is not the formal release package.

## Licensing

The application source is licensed under MIT. SQLite is public domain, and the pinned GoogleTest source uses BSD-3-Clause; both are fetched at pinned versions and are not copied into the repository. FFmpeg, Qt, NVIDIA libraries, RIFE code and weights require separate version/build-specific redistribution review before packaging. No model weights or vendor runtime binaries are included.

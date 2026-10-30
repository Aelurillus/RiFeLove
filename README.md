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
- CMake/Ninja builds and automated tests. The default `windows-release` preset builds the native desktop with the media and ONNX CUDA engines, with Qt and the developer CLI disabled. See [local native development](docs/development/LOCAL.md).

Video processing, models, settings, test reports and task databases stay on the local machine. Reviewed source changes can be committed and pushed to the project's Git repository; hosted CI and formal release publishing remain deferred. A future Windows CI template is kept under `docs/development/templates/` and is not active. The older Qt/QML Studio remains in the tree as legacy implementation work; its [Qt setup guide](docs/development/DESKTOP.md), scripts and source are not the current native development or delivery entry point. The CLI is an optional internal developer tool, not a product feature or bundled executable.

The current AI video adapter supports explicitly tagged BT.709 and BT.470BG/SMPTE 170M 8/10-bit SDR, using the tagged YUV matrix and preserving the source profile and pixel format when the selected encoder supports it. A narrowly verified H.264 VUI/container disagreement within the BT.601-family SDR tags is normalized to the container metadata when layout, bit depth, primaries and range match. The local native preview package contains the two default model packs described above; quality-ranked profiles and real-content comparison remain required. The isolated FFmpeg 8.1.3 SDK has been validated on the local GTX 1660 SUPER for H.264 NVENC, HEVC NVENC 8-bit/10-bit, CUDA/NVDEC decode, OpenH264, Kvazaar and libaom software paths, plus a native CUDA RIFE run. The packaged SDK still uses the no-x86asm build, so its CPU decode/software-encode throughput remains unqualified; a separate NASM-enabled x86asm rebuild passed real file, NVENC/NVDEC, native DLL, RIFE/CUDA and CPU A/B checks but remains a validation artifact until it is deliberately promoted. Automatic mode can fall back to a supported software encoder while RIFE inference stays on CUDA. Real-content 10-bit quality, full-resolution/content qualification, FP16, HDR processing, multi-hour checkpoint coverage, broader hardware qualification, quality-ranked model profiles, native queue/preview migration and an audited installer remain required. See the [pipeline evidence and limits](docs/development/PIPELINE.md), [desktop queue guide](docs/development/QUEUE.md), [SRS](docs/SRS.md) and [architecture](docs/architecture/ADD.md).

## Build

Use CMake 3.25+, Ninja and the supported MinGW C++20 toolchain on Windows x64. Set the following process-local environment variables to existing, pinned SDK directories before configuring the complete native release; no machine-specific path is stored in the preset:

```powershell
$env:RIFELOVE_FFMPEG_ROOT = '<local FFmpeg SDK root>'
$env:RIFELOVE_ONNX_ROOT = '<local ONNX Runtime 1.22.0 GPU SDK root>'
$env:RIFELOVE_TENSORRT_RUNTIME_DIR = '<local combined ONNX Runtime/TensorRT/CUDA/cuDNN DLL directory>'
$env:RIFELOVE_JSON_ROOT = '<local nlohmann JSON 3.12.0 root>'
.\tools\development\check.ps1 -Preset windows-release
```

This configures, builds and tests the native desktop, media and inference engines locally, writing `local-check.json` and `test-report.xml` into `build/windows-release/`. The release preset reads those four environment variables on each configure; explicit CMake `-D` arguments can override them. Missing SDKs or runtime DLLs fail configuration instead of producing an inspection-only desktop. Qt and CLI remain OFF. Python is used only by development checks; the desktop runtime has no Python dependency. MSVC/vcpkg and installer qualification remain pending.

Configure uses local dependency source caches by default. On a new offline machine, supply pinned spdlog, SQLite and GoogleTest sources through CMake's `FETCHCONTENT_SOURCE_DIR_SPDLOG`, `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` settings. Initial source acquisition is an explicit opt-in using `check.ps1 -Preset windows-release -AllowDependencyDownload`; it does not acquire the media/inference SDKs or publish project data. The `windows-dev` preset remains a reduced internal core/test build and is not a complete product build. See [local development](docs/development/LOCAL.md).

After a successful release build, create the local model/runtime bundle with `packaging/windows/package-native-local.ps1`, supplying `-FfmpegRoot`, `-InferenceRuntimeDirectory` and `-MinGWBinDirectory` for the selected local toolchain and SDKs. Launch the resulting `out/RifeLoveNative-local-nvenc/RifeLoveNative.exe`. The [native bundle guide](packaging/windows/NATIVE_LOCAL_BUNDLE.md) describes model requirements, checksums, startup validation and current distribution limits. Older Qt packaging scripts are legacy tooling and are not used for the native product.

A checksum-pinned FFmpeg setup helper and tested limitations are documented in [FFmpeg development](docs/development/FFMPEG.md). Normal builds do not acquire this SDK automatically.

The isolated source-build recipe for the NVIDIA-compatible SDK and its NASM/x86asm comparison is documented in [FFMPEG_SOURCE_BUILD.md](docs/development/FFMPEG_SOURCE_BUILD.md). It writes only under `work/` and is not the formal release package.

## Licensing

The application source is licensed under MIT. SQLite is public domain, and the pinned GoogleTest source uses BSD-3-Clause; both are fetched at pinned versions and are not copied into the repository. FFmpeg, Qt, NVIDIA libraries, RIFE code and weights require separate version/build-specific redistribution review before packaging. No model weights or vendor runtime binaries are included.

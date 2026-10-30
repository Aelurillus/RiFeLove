# Local native development

The current product is the Windows x64 native desktop application, `RifeLoveNative.exe`, using Win32, Direct2D and DirectWrite. Processing, model files, settings, task databases, logs and test evidence stay local. Reviewed source changes can be committed and pushed to the project's Git repository; hosted CI and formal release publishing remain deferred. The CI file in `templates/` is reference material only.

## Complete native release build

Use CMake 3.25+, Ninja and the supported MinGW C++20 toolchain. Put the selected compiler and Ninja on the current PowerShell process's `PATH`. The default `windows-release` preset explicitly enables the native desktop, FFmpeg media engine, ONNX CUDA inference engine and tests. It disables the legacy Qt desktop and developer CLI, and requires the processing service rather than allowing an inspection-only shell.

Set these variables to existing local SDK directories before configuring. The preset reads them on every configure so a stale CMake cache cannot silently choose an SDK from another workstation:

```powershell
$env:RIFELOVE_FFMPEG_ROOT = '<local FFmpeg SDK root>'
$env:RIFELOVE_ONNX_ROOT = '<local ONNX Runtime 1.22.0 GPU SDK root>'
$env:RIFELOVE_TENSORRT_RUNTIME_DIR = '<local combined ONNX Runtime/TensorRT/CUDA/cuDNN DLL directory>'
$env:RIFELOVE_JSON_ROOT = '<local nlohmann JSON 3.12.0 root>'
.\tools\development\check.ps1 -Preset windows-release
```

Run this example from the repository root, or invoke `check.ps1` by its full path from another directory. The script configures, builds and tests locally. For separate steps:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --parallel 4
ctest --preset windows-release --output-junit build/windows-release/test-report.xml
```

The four SDK variables can alternatively be supplied as explicit `-DRIFELOVE_...=<local path>` arguments to `cmake --preset windows-release`. `check.ps1` also accepts `-FFmpegRoot` and `-TensorRTRuntimeDirectory` overrides for those two locations. A missing SDK directory, required header or combined-runtime DLL fails configuration before dependency setup. Existing ONNX Runtime and JSON header hash checks still enforce their pinned versions; FFmpeg library discovery also remains required. SDK acquisition is never automatic.

| Variable | Expected local layout |
| --- | --- |
| `RIFELOVE_FFMPEG_ROOT` | FFmpeg SDK with `include/`, `lib/` and `bin/`; media integration checks also require `bin/ffmpeg.exe` and `bin/ffprobe.exe` |
| `RIFELOVE_ONNX_ROOT` | Pinned ONNX Runtime GPU SDK with `include/onnxruntime_c_api.h` and its runtime files under `lib/` |
| `RIFELOVE_TENSORRT_RUNTIME_DIR` | Combined ONNX Runtime core/providers, TensorRT 10, CUDA 12 and cuDNN 9 DLLs; a standalone ORT SDK is insufficient |
| `RIFELOVE_JSON_ROOT` | Pinned JSON source root containing `include/nlohmann/json.hpp` |

The optional `RIFELOVE_TEST_RIFE_PACK` CMake setting points to a qualified local reference pack and enables hardware model-reference tests; when it is configured, the selected combined runtime enables TensorRT FP32 reference checks as well. `RIFELOVE_ENABLE_TENSORRT_FP16_VALIDATION` remains opt-in. Successful compilation alone does not qualify a GPU, model or runtime for release.

The check writes these generated, Git-ignored reports:

- `build/windows-release/local-check.json`: result and exit code for configure, build and tests.
- `build/windows-release/test-report.xml`: JUnit test results.

Configure normally uses existing pinned spdlog, SQLite and GoogleTest source caches with downloads disabled. For a new offline build directory, pass `FETCHCONTENT_SOURCE_DIR_SPDLOG`, `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` through explicit CMake `-D` arguments. Otherwise first supply those sources locally, or explicitly permit one-time source acquisition with `check.ps1 -Preset windows-release -AllowDependencyDownload`. That switch does not acquire FFmpeg, ONNX Runtime, TensorRT, CUDA or model weights. Python is optional development/reference tooling and is not part of the application runtime. MSVC/vcpkg and installer qualification remain pending.

The `windows-dev` preset remains a reduced internal core/test build: media and ONNX CUDA are not enabled by default, and its native window may be inspection-only. It is useful for isolated core work but must not be packaged as the complete product.

## Local native bundle

After building and testing `windows-release`, package the existing local SDKs and model packs:

```powershell
.\packaging\windows\package-native-local.ps1 `
  -FfmpegRoot $env:RIFELOVE_FFMPEG_ROOT `
  -InferenceRuntimeDirectory $env:RIFELOVE_TENSORRT_RUNTIME_DIR `
  -MinGWBinDirectory '<selected MinGW compiler bin directory>'
```

The current packaging script has workstation defaults; pass explicit local paths as shown when using a different SDK/toolchain layout. Model pack parameters can also select existing local packs. The package script verifies the native executable's imports, model/sidecar hashes, required runtime DLLs, absence of Qt/QML/CLI files and startup. Its default output is `out/RifeLoveNative-local-nvenc/`; double-click its `RifeLoveNative.exe`. See [the native bundle guide](../../packaging/windows/NATIVE_LOCAL_BUNDLE.md) for the exact model layout, refresh safeguards and qualification limits. This is a local preview bundle, not an audited transferable installer. Weights and vendor runtime binaries stay out of Git.

## Queue and legacy tooling

Use the desktop queue for batch processing, checked retry/restart and checkpoint recovery. The shared configured queue acquires an OS ownership lock and binds checkpoints to source, model and processing identity. See [queue contracts and limits](QUEUE.md) for recovery semantics and remaining acceptance work.

`apps/desktop/`, `ui/`, `check-desktop.ps1`, `run-desktop.ps1`, `setup-qt.ps1` and the [Qt desktop guide](DESKTOP.md) describe the legacy Qt implementation; they are not prerequisites for the native build or bundle. `apps/cli/` and CLI-based integration/reference commands remain optional internal developer tooling behind `RIFELOVE_BUILD_CLI=ON`. They are not end-user product features and are not shipped. Several older pipeline/media guides still contain those internal commands; they do not change the native product scope. Do not run legacy task-maintenance commands against an active configured-queue worker.

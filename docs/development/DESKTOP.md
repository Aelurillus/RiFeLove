# Local Qt desktop

The first connected Qt/QML Studio now imports one local file (picker or drag/drop), inspects video/GPU information, selects target FPS/encoder/output/model/runtime directories, runs the native pipeline and cancels cooperatively. Source files are preserved and existing outputs are rejected. Work runs through an application service on a bounded background worker; QML owns no FFmpeg, CUDA or ONNX contexts. The main event loop remains available during processing.

This is still a development workflow. Model/runtime directories are explicit local selections. A full Model Center, persistent settings, multi-file queue, profiles, pause/retry/checkpoints, comparison playback, all four languages and release packaging remain required. The UI currently uses Simplified Chinese. No network acquisition occurs inside the desktop application.

## SDK

`tools/development/qt-sdk.lock.json` pins the local Qt 6.12 archives and GCC 15.1 MSVCRT toolchain. Qt archive hashes were checked against publisher SHA-1 and pinned with SHA-256; the compiler's SHA-256 is a first-acquisition pin from its upstream release, without a separate publisher digest. These checks establish reproducibility, not an authenticity signature.

The Qt repository metadata still names a 13.1 compiler dependency; the actual `config_qtbase.summary` identifies GCC 15.1 and Qt6Core imports MSVCRT. The desktop therefore uses a separate matching GCC 15.1 toolchain, without changing the existing GCC 16 core/CLI builds or the system PATH.

The MSVCRT compiler's out-of-line `_Exit` stub references UCRT-only imports when GoogleTest's Debug death-test support links. A test-only compatibility definition maps the standard immediate-exit operation to MSVCRT `_exit`; a child-process exit-status regression verifies it. Product binaries do not contain this definition and no second CRT is linked to repair the test.

```powershell
# Offline cache verification of the already installed local SDK
.\tools\development\setup-qt.ps1 -QtRoot C:/itzming/dependences/qt-6.12.0-local -VerifyOnly

# Extract checksum-pinned cached archives to an isolated SDK directory
.\tools\development\setup-qt.ps1 -QtRoot ./work/dependencies/qt-6.12.0
```

Acquisition requires explicit `-AllowDownload`; normal setup/checks stay offline. The helper changes only its selected SDK directory and process-local environment. Qt module, third-party and compiler runtime source/notice obligations still need the release redistribution audit; this development SDK is not an audited application bundle. Qt's [LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations) describe the upstream requirements to evaluate before shipping.

## Build, test and open

```powershell
.\tools\development\check-desktop.ps1 `
  -QtRoot C:/itzming/dependences/qt-6.12.0-local `
  -FFmpegRoot C:/itzming/dependences/ffmpeg-8.1.3/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1 `
  -OnnxRoot C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0 `
  -JsonRoot C:/itzming/dependences/nlohmann-json-3.12.0 `
  -ModelPack C:/itzming/rifelove/models/cache/rife-4.25-fp32-sdr8-v2 `
  -ReferencePython C:/itzming/rifelove/work/model-tools/Scripts/python.exe

.\tools\development\run-desktop.ps1 -QtRoot C:/itzming/dependences/qt-6.12.0-local
```

Use `-Configuration Release` for the separate `build/windows-qt-release` directory. `SourceCache` can override the default SQLite/GoogleTest cache in `build/windows-dev/_deps`. The check requires Qt, configures offline, bounds build parallelism and writes local JSON/JUnit reports. Python is used only by development checks; the desktop executable never starts it.

The startup test renders the compiled QML. The workflow test loads the same QML source, activates its actual start button, generates native RIFE output, observes a responsive GUI event loop, rejects repeated output and cancels a subsequent job. A separate FFmpeg/ffprobe process checks decoded frame count, bit-identical decoded audio and unchanged source hash. The test preserves a real completed-job screenshot and JSON evidence under the build directory. Automated font rendering uses the Windows font directory with Qt's offscreen platform; ordinary application launch uses the Windows platform.

Current restrictions match the [pipeline](PIPELINE.md): explicitly tagged BT.709 8-bit SDR and the qualified local model pack. This machine's SDK/driver cannot initialize NVENC; automatic H.264 selection reports its actual software fallback. Full product quality, hardware matrix, HDR and long-duration acceptance are still open.

The shared native runtime/environment now lasts for the process. After its first use, changing the ONNX/CUDA runtime directory requires restarting RifeLove. The new [persistent queue service and CLI](QUEUE.md) are not yet exposed as a desktop queue page.

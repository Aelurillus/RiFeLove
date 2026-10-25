# Local Qt desktop

The Qt/QML desktop now has a single-file Studio and a persistent local Task Queue. The queue accepts multiple files or folders (optional recursive scan), screens media against the active RIFE color contract, deduplicates sources, allocates non-overwriting names, and reports skipped HDR/unsupported inputs. It stores tasks in SQLite under the Windows local application data directory (or `--data-dir`) and stores last-used FPS, model, runtime, output, encoder and container settings in a local `preferences.ini`. The QML controls expose sequential start, safe pause/resume, stop, per-task cancellation, failed retry, interrupted restart-from-zero and opening completed output folders. Studio and queue rows estimate total output frames and remaining time from source duration and target frame rate; they label this estimate separately from measured processing FPS. Source files are preserved. Services own media and GPU work on background threads; QML owns no FFmpeg, CUDA or ONNX contexts.

This is still a development workflow, not the complete product. The offline Model Center registers local model directories and verifies each model descriptor, tensor contract, bundled license and ONNX SHA-256 before selection; the catalog and default model persist locally and are revalidated on refresh. The workstation run folder bundles RIFE 4.25 and 4.26 FP32 SDR8 packs and selects 4.25 by default. Both pass small CPU/CUDA/TensorRT numerical checks; only 4.25 has the end-to-end video comparison. Model downloads, a trusted provenance registry, quality-ranked presets and real-content comparisons remain required. The queue persists task state and preferences, but long-term history pagination, cross-process active control, safe stale-artifact cleanup, artifact navigation, all four languages and a redistributable installer remain required. The comparison page locally opens the source and completed output side by side, synchronizes playback and mutes result audio. While paused, it seeks through exact FFmpeg-decoded tagged SDR frames; the regression workflow checks source/output image loading and forward/backward CFR and VFR frame steps. HDR and unknown-color still previews remain blocked until a validated display transform exists. The UI currently uses Simplified Chinese. No network acquisition occurs inside the desktop application.

## Run the local workstation bundle

On this workstation, build and launch the self-contained app folder with:

```powershell
.\packaging\windows\package-local.ps1
```

The script writes `out/RifeLove-local/`, deploys Qt and FFmpeg runtime files, copies the desktop and CLI programs plus selected RIFE model packs, creates first-run local preferences, launches a desktop startup smoke check, captures `first-run.png`, and checks that the CLI exposes interpolation and queue commands. Run `out/RifeLove-local/RifeLove.exe` for the desktop app or `out/RifeLove-local/rifelove-cli.exe --help` for command usage. The folder is ignored by Git because it contains build outputs and model weights.

At startup, a missing or invalid saved model/runtime path falls back to the verified model and CUDA runtime bundled beside the app. This repairs stale local settings without resetting saved FPS, encoder, output or task history.

This is a local runnable folder rather than a transferable release: its `runtime` directory links to the ONNX Runtime/TensorRT/CUDA/cuDNN libraries installed on this machine. It can bundle multiple model packs; this workstation's package defaults to 4.25 and also includes 4.26. App settings and task history are stored under the bundle's `data` folder; no network access is needed for normal use.

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

The startup test renders the compiled QML against an isolated build-local data directory. The workflow test loads the same QML, activates the Studio start button, imports a recursive folder plus duplicate source selections, reports an HDR input rejected by the current model contract, processes colliding same-basename videos without overwriting, and exercises queue pause/resume. It also reloads saved preferences, forces an output collision to verify failure/retry, and cancels an individual queued task while checking the corresponding row controls and persistent states. A separate FFmpeg/ffprobe process checks frame counts, bit-identical decoded audio and unchanged source hash. Real Studio and queue screenshots plus JSON evidence stay under the build directory. Automated font rendering uses the Windows font directory with Qt's offscreen platform; ordinary application launch uses the Windows platform.

Current restrictions match the [pipeline](PIPELINE.md): explicitly tagged BT.709 or BT.470BG/SMPTE 170M 8/10-bit SDR and a local model pack declaring the corresponding input profile. The Studio readiness label agrees with its start gate for unsupported color, missing GPU, incompatible encoder or missing local model/runtime. Import shows source-format encoder capabilities; immediately before model loading, Studio opens the selected encoder using the requested output frame rate, time base, pixel format, bitrate, GOP and options. Automatic software encoding selects a compatible H.264, HEVC or AV1 profile and the UI identifies CPU encoding while RIFE remains on CUDA. This machine's SDK/driver cannot initialize NVENC; the local FFmpeg requires NVENC API 13.0 / driver 570.0 or newer, while the installed driver provides API 12.2. Full visual quality, hardware matrix, HDR and long-duration acceptance remain open.

The shared native runtime/environment now lasts for the process. After its first use, changing the ONNX/CUDA runtime directory requires restarting RifeLove. Queue tasks pin their model and job settings; changing a queued job means adding a separate task. “Stop queue” cancels the active job at a safe boundary and preserves later queued jobs. Interrupted output is never silently resumed; selecting “restart from beginning” creates a separate task/output. See [queue semantics](QUEUE.md).

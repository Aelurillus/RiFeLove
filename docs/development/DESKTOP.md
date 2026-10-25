# Local Qt desktop

The Qt/QML desktop now has a single-file Studio and a persistent local Task Queue. The queue accepts multiple files or folders (optional recursive scan), screens media against the active RIFE color contract, deduplicates sources, allocates non-overwriting names, and reports skipped HDR/unsupported inputs. It stores tasks in SQLite under the Windows local application data directory (or `--data-dir`) and stores last-used FPS, model, runtime, output, inference backend, encoder and container settings in a local `preferences.ini`. The inference choice can prefer TensorRT with CUDA fallback, require CUDA only or require TensorRT; processing stays FP32 pending FP16 qualification. The QML controls expose sequential start, safe pause/resume, stop, per-task cancellation, failed retry, interrupted restart-from-zero and opening completed output folders. Studio and queue rows estimate total output frames and remaining time from source duration and target frame rate; they label this estimate separately from measured processing FPS. Source files are preserved. Services own media and GPU work on background threads; QML owns no FFmpeg, CUDA or ONNX contexts.

This is still a development workflow, not the complete product. The offline Model Center registers and verifies local RIFE packs and separate video-restoration packs. RIFE defaults require an actual verified RIFE family; restoration packs show their declared task, temporal window and scale, but cannot be run yet. Unregistering a pack leaves its local files in place. The workstation package includes RIFE 4.25, 4.25 Lite and 4.26 FP32 SDR8 packs while keeping 4.25 as the default. All three pass small CPU/CUDA/TensorRT numerical checks; 4.25 has an independent end-to-end PyTorch video comparison, and Lite has only a synthetic 720p benchmark, not a real-content quality ranking. Model downloads, a trusted provenance registry, qualified restoration inference, quality-ranked presets and real-content comparisons remain required. The queue persists task state and preferences, verifies and resumes committed video segments after process interruption, and removes unjournaled task-owned segment artifacts on explicit resume. Long-term history pagination, cross-process active control, full keyboard/accessibility coverage and a redistributable installer remain required. The comparison page locally opens the source and completed output side by side, synchronizes playback and mutes result audio. While paused, it seeks through exact FFmpeg-decoded tagged SDR frames and steps by the actual CFR/VFR presentation timestamps. Separate source and result actions open their containing folders; a Qt workflow verifies both paths without launching the OS file manager. HDR and unknown-color still previews remain blocked until a validated display transform exists. The desktop ships offline zh-CN, zh-TW, en and ja translations with a local language setting; the selected language takes effect after restart. See [LOCALIZATION.md](LOCALIZATION.md) for catalog maintenance and checks. No network acquisition occurs inside the desktop application.

## Run the local workstation bundle

On this workstation, build and launch the self-contained app folder with:

```powershell
.\packaging\windows\package-local.ps1
```

The script writes `out/RifeLove-local/`, deploys Qt and FFmpeg runtime files, copies the desktop and CLI programs plus selected RIFE model packs, creates first-run local preferences, launches home and Studio smoke checks, captures `first-run.png` and `studio-smoke.png`, and checks that the CLI exposes interpolation and queue commands. Run `out/RifeLove-local/RifeLove.exe` for the desktop app or `out/RifeLove-local/rifelove-cli.exe --help` for command usage. The folder is ignored by Git because it contains build outputs and model weights.

Package refreshes build and qualify in a unique sibling staging directory. The existing folder is not moved until the desktop smoke check, CLI check and local TensorRT model qualification all pass. Immediately before switching, the script refuses to replace a package whose desktop or CLI process is still running. It carries forward local preferences, language, SQLite queue/WAL, checkpoint artifacts, logs and user-installed model packs; the new package's TensorRT qualification evidence replaces stale evidence, and a newly bundled copy wins when its model ID already exists. Promotion is a same-volume directory rename with rollback to the previous folder if the new folder cannot be moved into place. A failed build leaves the current package available and the staging folder available for diagnosis. `SHA256SUMS.txt` covers every static package file and file under the linked local inference runtime, plus the TensorRT qualification record; mutable queue, preference and log data are excluded.

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

The startup test renders the compiled QML against an isolated build-local data directory. `--smoke-page home|studio|queue|models|comparison` selects a page for local screenshot capture; CTest captures both the home and Studio page so their first-run layouts stay reviewable. The workflow test loads the same QML, activates the Studio start button, imports a recursive folder plus duplicate source selections, reports an HDR input rejected by the current model contract, processes colliding same-basename videos without overwriting, and exercises queue pause/resume. It also reloads saved preferences, forces an output collision to verify failure/retry, and cancels an individual queued task while checking the corresponding row controls and persistent states. A separate FFmpeg/ffprobe process checks frame counts, bit-identical decoded audio and unchanged source hash. Real Studio and queue screenshots plus JSON evidence stay under the build directory. Automated font rendering uses the Windows font directory with Qt's offscreen platform; ordinary application launch uses the Windows platform.

Current restrictions match the [pipeline](PIPELINE.md): explicitly tagged BT.709 or BT.470BG/SMPTE 170M 8/10-bit SDR and a local model pack declaring the corresponding input profile. The Studio readiness label agrees with its start gate for unsupported color, missing GPU, incompatible encoder or missing local model/runtime. Import shows source-format encoder capabilities; immediately before model loading, Studio opens the selected encoder using the requested output frame rate, time base, pixel format, bitrate, GOP and options. Automatic software encoding selects a compatible H.264, HEVC or AV1 profile and the UI identifies CPU encoding while RIFE remains on CUDA. This machine's SDK/driver cannot initialize NVENC; the local FFmpeg requires NVENC API 13.0 / driver 570.0 or newer, while the installed driver provides API 12.2. Full visual quality, hardware matrix, HDR and long-duration acceptance remain open.

The shared native runtime/environment now lasts for the process. After its first use, changing the ONNX/CUDA runtime directory requires restarting RifeLove. Queue tasks pin their model and job settings; changing a queued job means adding a separate task. “Stop queue” cancels the active job at a safe boundary and preserves later queued jobs. Interrupted output is never silently resumed; selecting “restart from beginning” creates a separate task/output. See [queue semantics](QUEUE.md).

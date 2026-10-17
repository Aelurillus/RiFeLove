# RifeLove Master Plan V1.0

Baseline: 2026-10-17. This is the complete product scope, not an MVP commitment. A phase can be marked complete only when its acceptance conditions and tests pass; a working foundation does not imply the product is finished.

## Product and architecture

Build an offline Windows 10/11 x64 desktop application for NVIDIA GPUs that balances visual quality, performance, stability and ease of use. Preserve source files; write a new output by default. Keep the UI, application services, core pipeline, media engine, timeline, interpolation, quality analysis, GPU runtime and inference backends behind explicit interfaces. The UI must submit work through services and never call CUDA or FFmpeg directly.

Final stack: C++20, Qt 6/QML, CMake/Ninja, vcpkg, FFmpeg libraries, CUDA, TensorRT, ONNX Runtime CUDA, RIFE, SQLite, spdlog, GoogleTest, pytest, GitHub Actions, and WiX or Inno Setup. Python/PyTorch belong only in model development/export and validation tools. Review every third-party library, model source and NVIDIA runtime for its exact license and redistribution conditions.

## Final capability scope

P0: drag/drop, file/folder batch import; probe resolution, CFR/VFR, HDR, codecs, audio and subtitles; multiple RIFE models; arbitrary rational target rates and 2x/4x/8x; CUDA, TensorRT, FP16 and bounded VRAM; MP4/MKV/MOV with H.264/HEVC/AV1 when supported; audio passthrough, multiple tracks and sync; scene-cut and abnormal-motion handling; queue, pause, cancel, retry and resume.

P1: source/output preview, split comparison and frame stepping; model downloads, checksums, switching and versions; CRF/CQ, bitrate, color, 10-bit and HDR controls; automatic precision, batch and buffer choices; headless CLI with JSON progress.

P2: artifact/performance reports, model and pre/post-processing extension interfaces, and Simplified Chinese, Traditional Chinese, English and Japanese localization.

Quality profiles are Fast, Balanced, Quality and Custom; they describe policies rather than claiming a single model always wins. Advanced controls include backend, inference precision, color, VRAM budget, encoder and cut threshold, with explanations and reset-to-default support.

## Engine behavior

Probe all stream, time-base, HDR, audio and subtitle metadata. Decode/reorder using timestamps; preprocess color and pixels; schedule output times; analyze cuts/abnormal frames; interpolate; postprocess; encode; mux and validate duration, timestamps, frame count and stream integrity. Represent rates and timestamps as rational/integer values to prevent drift. Support 23.976/29.97/59.94, VFR input, CFR or source-like output, integer and non-integer conversions, and explicitly test the final frame, audio boundary and mux time base. Preserve audio by stream copy where possible; handle multiple tracks, subtitles, chapters, language tags, rotation and container incompatibility.

Use a model registry containing model ID/version, source, license, SHA-256, capabilities and use cases. For each candidate: audit license; establish PyTorch reference output; export ONNX; validate dynamic timestep/resolution and operator support; compare ONNX CUDA output; build TensorRT plans; evaluate FP16, speed and VRAM. Key engine caches by GPU architecture, TensorRT/model versions and optimization options; don't distribute a developer-machine engine as universal. Plan CUDA streams/graphs, reusable buffers, resolution profiles, memory budgets and optional overlapped tiling only after correct reference output and a performance baseline exist.

Quality protection covers scene-cut ghosts, fast-motion deformation, occlusion artifacts, duplicate frames, animation line warping, text/HUD distortions, flicker, HDR conversion, borders and tiling seams. Flag uncertain regions and prefer safe behavior over claiming imperceptible output. Use NVDEC/NVENC only when capability checks pass, with CPU decode/encode fallbacks as configured. Measure end-to-end throughput, memory and reliability by GPU/model/resolution, not just inference latency. Never promise real-time 4K/120 processing across all NVIDIA cards.

Tasks persist configuration, model version, file identity/checksum, state and safe segment records. Pause/resume and crash recovery are different: true checkpointing requires safe encoded segment boundaries, timestamps and valid stitching. A startup recovery pass must make interrupted state visible; never claim resumability from a frame index alone.

## User experience and quality goals

Provide Home/Workspace, Interpolation Studio, Comparison Viewer, Task Queue and Model Center pages. Normal path: choose video, target FPS, quality and output. Expert controls are expandable. UI reports output FPS separately from processing speed and GPU/memory telemetry when available.

Test resolutions 480p through 4K; input 23.976 through VFR; output 60/120/144/240/custom; real footage, games, animation and capture; slow/fast motion, occlusion and cuts; H.264/HEVC/AV1; SDR/HDR and 8/10-bit; short to multi-hour clips; corrupt files, disk exhaustion, OOM and crashes. Use PSNR/SSIM/LPIPS only when ground truth exists; otherwise use blind review, temporal checks and artifact flags. Release goals include 99.9% success on a controlled compatibility suite, no unreported sync errors, detectable state after abnormal exit, repeatable same-hardware benchmarks, full P0 acceptance, tested recovery, no known high-severity security/data-loss defects, installer/uninstaller/model-download/first-run validation and completed license review. These are goals, not measured claims.

## Repository map

`apps/{desktop,cli}`, `src/{core,media,inference,interpolation,gpu,tasks,database,common}`, `ui/{pages,components,themes,translations}`, `models/{manifests,adapters}`, `tools/{model_export,benchmarking,validation}`, `tests/{unit,integration,quality,performance,fixtures}`, `packaging/windows`, `docs/{architecture,api,development,user-guide}` and `.github/workflows`. Keep generated builds, downloaded weights and local TensorRT cache out of Git.

## Phases and exit criteria

1. **Engineering foundation and core architecture** — CMake, Qt, logging, config, task model, interfaces, CI and tests. Exit: foundation builds and tests.
2. **Media engine** — FFmpeg demux/decode, timeline, encode, audio/subtitle, metadata and validation. Exit: media correctness suite passes.
3. **AI interpolation** — audited RIFE adapters, ONNX CUDA, TensorRT, arbitrary-time inference and model registry. Exit: real end-to-end AI interpolation.
4. **GPU performance** — NVDEC/NVENC, memory pool, asynchronous pipeline, engine cache and profiling. Exit: reproducible performance baseline.
5. **Quality and color** — scene detection, artifact policy, HDR/SDR, 10-bit, VFR and difficult content. Exit: quality regression suite passes.
6. **Desktop experience** — QML workflow, preview, queue, model center, presets and localization. Exit: complete desktop workflow.
7. **Reliability and pro features** — recovery, retry, CLI, reports, advanced encoding and quality analysis. Exit: long-video and fault-recovery suite.
8. **Release and maintenance** — installer, signatures plan, license/security reviews, docs and release automation. Exit: stable release and maintenance process.

The schedule can overlap UI and engine work. Do not spend heavily on CUDA micro-optimization before model correctness. Final completion means desktop and CLI, real RIFE GPU interpolation, full media/audio/subtitle/color handling, queue/recovery/model management, preview/quality/encoding controls, tests/benchmarks/docs, installable release, license compliance and update mechanism are all accepted.

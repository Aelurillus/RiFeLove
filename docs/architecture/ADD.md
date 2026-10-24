# Architecture design document

Version 0.1. This document records implemented boundaries and the remaining architecture required by the complete Master Plan.

## Layering

Desktop and CLI adapters submit application-service operations. Task, profile, model, device and progress services own user-facing state and orchestration. The core pipeline schedules rational presentation times and coordinates media, quality, interpolation and GPU resources. Backend implementations own FFmpeg/CUDA/ONNX/TensorRT contexts. UI code must not own those contexts.

The CLI adapter calls media and native video pipeline public C++ APIs. The desktop's `StudioController` marshals owned values between QML and the standard-C++ `application::ProcessingService`. `QueueController` and its Qt worker wrap `application::TaskQueue` and the shared `ImportService`; the UI gets a `QAbstractListModel` projection and never calls media/GPU code. The queue worker owns database access and job execution off the GUI thread, while callbacks return immutable values to the controller thread. No vendor context is owned by QML or either controller. The offline Model Center uses the same worker boundary to verify local packs and persist the catalog/default selection. RIFE pack schema v2 verifies model, license, source-manifest and validation-report hashes and exposes the declared license, upstream commit, numerical profile, input profiles and validated backends. Legacy v1 packs remain readable and are visibly identified as lacking sidecar hash binding. The hashes provide local consistency checks, not source authentication; downloads, a trusted provenance registry, multiple qualified profiles, advanced preferences and complete task-history management remain required.

`application::TaskQueue` owns configured-task execution and live database ownership. The common executor admits one job at a time per process and protects source bytes with a native read lease. `task_details` stores versioned configuration/identity/progress/result payloads beside the existing task state machine. Batch insertion and state/result publication are transactional; progress is throttled. Jobs pin model descriptors when queued and retain checked source/model identity after preparation. `ImportService` performs bounded local file/folder discovery, stable path deduplication, output allocation, probe/color eligibility checks and stop-aware atomic enqueue. Recovery requires the OS ownership lock, marks active configured work interrupted, and preserves history. See [queue contracts](../development/QUEUE.md) for pause/cancel/retry and explicit restart semantics. Desktop model/profile orchestration, full audit/history pagination and safe segment checkpoints remain incomplete.

## Implemented media contract

`MediaEngine::probe` returns stream/container/chapter metadata in standard C++ values. `decodeVideo` yields `DecodedFrame` objects with rational PTS, optional duration, native pixel planes, color and reference-counted ownership. Retained frames remain valid after decoder closure. The interface does not require callers to include FFmpeg headers. Missing timestamps are explicit, and observed cadence accounts for input-time-base quantization.

Input I/O is restricted to local regular files and the `file` protocol. Cancellation uses a stop token, interrupt callbacks and boundary checks. FFmpeg errors carry both operation text and the native error code. The current decoder is software-only; CUDA frame surfaces and transfers will be separate from CPU planes.

`remux` copies existing compressed streams and metadata; it does not perform interpolation or video encoding. Timestamp rescaling occurs after mux header creation, when final output time bases are known. Incompatible streams fail visibly. Output is written under a unique same-directory reservation, finalized and structurally probed, then atomically published without replacement on Windows. Exceptions/cancellation clean up the temporary file. Process-kill recovery requires the future checkpoint/ownership system.

`VideoMuxWriter` accepts owned CPU pixel planes at explicit rational timestamps/durations, encodes the selected video and copies the reference input's other streams. The time base must represent submitted times exactly; overlapping or unknown timestamps are rejected. Encoder output is drained in DTS order, with submitted PTS/duration associations retained through reordering. The pending association table is capped at 1024 entries. A separate input handle pumps auxiliary packets according to source-video DTS so they need not be retained for the entire job. This implementation prioritizes correctness; shared-demux and GPU-surface optimizations remain required.

Encoding copies color properties to both frames and encoder context, rebuilds available static HDR frame metadata and retains codec-independent stream side data. Pre-publication checks verify stream presence, output bit depth and known color tags. Implicit precision reduction is rejected; HDR bit-depth reduction and RGB/YUV color transforms require the future validated color pipeline. Basic same-color pixel layout changes use libswscale. The selected encoder must open successfully; a registered encoder name is not a hardware capability claim. See `docs/development/ENCODING.md` for the current NVENC incompatibility.

## Implemented interpolation pipeline

`core/pipeline/video_pipeline` now joins adjacent decoded frames, the integer-index rational CFR clock, SDR conversion, conservative scene analysis, real asynchronous ONNX CUDA inference and the frame writer. It owns no UI state. Inputs remain bounded to adjacent frames and one pending output; exclusive GPU output buffers can be reclaimed only after all immutable references have gone. Source-coincident frames bypass inference. Held cut/duplicate/abnormal pairs are separately reported.

The last output duration preserves the source's exclusive end. Full output re-decode, frame-count/PTS checks, copied-audio boundaries and color checks run inside the writer's prepublication callback. The `auto` encoder policy tests actual NVENC initialization and records the specific software fallback reason. Explicit codec requests stay strict. AAC priming/Matroska CodecDelay and final-packet presentation duration are preserved, with decoded-PCM regression evidence. See [pipeline contracts](../development/PIPELINE.md).

## Required next contracts

- GPU-surface encoder submission, source-shared demux, comprehensive color conversions and full compatibility/fault coverage for the implemented CPU-frame writer.
- Expand the integrated native CUDA numerical/operator and video-reference tests to the full resolution/content/color matrix, plus TensorRT and qualified model presets.
- Extend the checksum-bound RIFE v2 pack schema into a trusted multi-family model registry, with separately reviewed weight rights, qualified quality profiles and model/update versioning.
- A bounded scheduler with explicit buffer ownership and backpressure across decode, preprocessing, quality, inference, postprocessing and encode stages.
- Persistent task configuration/progress and safe output-segment checkpoints bound to source/model/build identity. The manifest/journal and video-only segment writer are implemented; pipeline integration, on-disk verification, resume and stitching must satisfy [the checkpoint contract](SEGMENT_CHECKPOINTS.md). Restart recovery must establish worker ownership before marking tasks interrupted.
- A color contract defining transfer/primaries/range/matrix, precision, HDR side data and conversion policy at every stage. Do not equate retained tags with a validated HDR neural processing pipeline.

## Validation mapping

`tests/unit/` covers rational scheduling, task-store semantics, frame ownership and stop behavior. `tests/integration/media_test.py` generates synthetic video/audio/subtitle/chapter fixtures and compares native operations to separately invoked FFmpeg tools. Tests and their limits are documented in `docs/development/FFMPEG.md`. Full SRS and release validation remain open.

The CUDA driver module separately owns context/stream/buffer lifetime. The ONNX adapter uses a bounded single-worker queue and prebound device outputs; its public interface carries no vendor API types. Model loading checks bounded files, hashes and tensor shapes. Runtime dependencies, small-profile numerical/operator evidence and the remaining hardware/resolution matrix are detailed in `docs/development/GPU.md`; CPU export qualification is deliberately scoped in `docs/development/RIFE_MODELS.md`.

The media service exposes source-profile encoder context probes separately from FFmpeg's encoder registry. Studio uses this at import to report whether automatic H.264 can initialize NVENC or must use software H.264; the diagnostic CLI reports H.264, HEVC and AV1 candidates. Context initialization is only a preflight signal. Encoded-frame success is still established by the real job's finalized-output validation, and NVDEC remains a separate unimplemented decoder path.

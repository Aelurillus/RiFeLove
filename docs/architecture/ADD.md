# Architecture design document

Version 0.1. This document records implemented boundaries and the remaining architecture required by the complete Master Plan.

## Layering

Desktop and CLI adapters submit application-service operations. Task, profile, model, device and progress services own user-facing state and orchestration. The core pipeline schedules rational presentation times and coordinates media, quality, interpolation and GPU resources. Backend implementations own FFmpeg/CUDA/ONNX/TensorRT contexts. UI code must not own those contexts.

The current CLI adapter calls the media engine's C++ public API. Task/profile/model/device orchestration services are still to be built; the current adapter is not the finished services layer. The Qt shell is not connected to video processing.

## Implemented media contract

`MediaEngine::probe` returns stream/container/chapter metadata in standard C++ values. `decodeVideo` yields `DecodedFrame` objects with rational PTS, optional duration, native pixel planes, color and reference-counted ownership. Retained frames remain valid after decoder closure. The interface does not require callers to include FFmpeg headers. Missing timestamps are explicit, and observed cadence accounts for input-time-base quantization.

Input I/O is restricted to local regular files and the `file` protocol. Cancellation uses a stop token, interrupt callbacks and boundary checks. FFmpeg errors carry both operation text and the native error code. The current decoder is software-only; CUDA frame surfaces and transfers will be separate from CPU planes.

`remux` copies existing compressed streams and metadata; it does not perform interpolation or video encoding. Timestamp rescaling occurs after mux header creation, when final output time bases are known. Incompatible streams fail visibly. Output is written under a unique same-directory reservation, finalized and structurally probed, then atomically published without replacement on Windows. Exceptions/cancellation clean up the temporary file. Process-kill recovery requires the future checkpoint/ownership system.

## Required next contracts

- An encoder/mux writer that accepts generated frames and copies compatible auxiliary streams while preserving the source timeline and color metadata.
- An asynchronous interpolation request carrying two owned GPU frames, rationally scheduled timestep, device/stream context, destination buffers and stop/error handling.
- A model registry/adaptor schema with checksum-verifiable provenance, exact license/weight rights, tensor layouts, shape/timestep/precision capabilities and versioned cache keys.
- A bounded scheduler with explicit buffer ownership and backpressure across decode, preprocessing, quality, inference, postprocessing and encode stages.
- Persistent task configuration/progress and safe output-segment checkpoints bound to source/model/build identity. Restart recovery must establish worker ownership before marking tasks interrupted.
- A color contract defining transfer/primaries/range/matrix, precision, HDR side data and conversion policy at every stage. Do not equate retained tags with a validated HDR neural processing pipeline.

## Validation mapping

`tests/unit/` covers rational scheduling, task-store semantics, frame ownership and stop behavior. `tests/integration/media_test.py` generates synthetic video/audio/subtitle/chapter fixtures and compares native operations to separately invoked FFmpeg tools. Tests and their limits are documented in `docs/development/FFMPEG.md`. Full SRS and release validation remain open.

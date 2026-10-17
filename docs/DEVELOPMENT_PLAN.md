# Executable development plan

Status as of 2026-10-17. This task list expands the Master Plan into reviewable work. It is intentionally not a declaration that the final product is complete.

Current execution mode: local project, local Git, local builds/tests and local reports. Hosted CI, remote repository setup and publishing are deferred until requested. The final product scope remains the Master Plan baseline.

## Phase 1 — foundation (active)

- [x] Create C++20/CMake/Ninja layout and reproducible configure presets.
- [x] Add exact rational values and a timestamp-based output timeline planner.
- [x] Add SQLite task schema, guarded state transitions and crash-state recovery.
- [x] Add CLI entry points for timeline plans and persisted tasks.
- [x] Add unit tests for arithmetic, VFR/non-integer timeline mapping and task transitions.
- [ ] Add structured logging/configuration, service/backend interfaces and Qt app shell.
- [x] Add offline-by-default local build/test runner and local JSON/JUnit reports.
- [x] Add read-only queue inspection and Windows Unicode CLI regression coverage.
- [x] Preserve the original complete Master Plan and add SRS/ADD requirement evidence documents.
- [ ] Activate hosted Windows CI when remote development is requested (template prepared).
- [ ] Validate MSVC + Qt + vcpkg release-toolchain configuration locally.

## Phase 2 — media engine (active, incomplete)

- [x] Bind a pinned local FFmpeg shared SDK and probe streams, rational time bases, metadata and color.
- [x] Decode software frames with presentation reordering/drain and retained original-precision storage.
- [x] Scan actual timestamp cadence with fractional-CFR quantization handling and VFR fixtures.
- [x] Stream-copy remux MP4/MOV/MKV with compatible audio/subtitles, chapters, no-overwrite publication and cancellation API.
- [ ] Complete codec/container coverage, missing-PTS recovery and full mastering/rotation metadata fixture coverage.
- [ ] Add explicit color/pixel-format and HDR metadata handling.
- [x] Add the first explicit tagged BT.709 8-bit SDR conversion to normalized RGB, replicated padding and restored crop; reject unknown/HDR/higher-depth inputs in this path.
- [x] Add a model-independent CPU-frame encoder/mux writer and transcode path with compatible original audio/subtitle stream copy.
- [x] Validate software H.264/HEVC/AV1, FFV1 lossless/HDR, encoder B-frame drain, VFR and exact 60000/1001 scheduling across 1000 generated frames.
- [ ] Validate duration, frame count, PTS monotonicity, audio sync and container integrity.
- [x] Add synthetic media integration tests for 10-bit/PQ, tracks/chapters, VFR/B-frames, packet hashes/timing, Unicode paths, corrupt files and output protection.
- [ ] Add the complete licensed fixture corpus, long-video and disk-full fault-injection suite.

## Phase 3 — interpolation

- [x] Verify the author's MIT scope statement and pin/acquire RIFE 4.25 network, checkpoint, warp source and license files.
- [ ] Complete the multi-model provenance audit and quality/profile selection.
- [x] Define the first async interpolation/backend contract with owned GPU inputs, bounded queue, cancellation and exclusive reusable outputs; 36 native reference/contract checks pass (full concurrency/fault stress pending).
- [x] Build PyTorch reference exporter and numerical comparison harness; separately record strict-gate failure and SDR8 numerical qualification.
- [ ] Integrate ONNX Runtime CUDA and validate supported model operators/shapes.
  - RIFE 4.25 native execution passes six small-profile SDR8 comparisons with CUDA Conv/ConvTranspose/GridSample placement; larger profiles/visual validation remain open.
- [ ] Integrate TensorRT backend and version/GPU keyed engine cache.
- [ ] Register checksum-verifiable model manifests with capability and license metadata.
- [ ] Pass real end-to-end video interpolation and forward quality tests.

## Phase 4 — GPU throughput

- [x] Add native CUDA device/context/stream/buffer primitives with actual local GPU transfer, budget and lifetime tests.
- [ ] Probe GPU/driver/compute and codec capabilities at runtime.
- [ ] Add NVDEC/NVENC selection with tested fallback paths.
  - Local NVENC probe currently fails: SDK requires API 13.0; installed driver exposes 12.2. This is an unresolved compatibility item, not a GPU-success result.
- [ ] Add bounded VRAM pool, CUDA streams and safe pipeline overlap.
- [ ] Benchmark cold/warm inference and end-to-end throughput by GPU/resolution.
- [ ] Evaluate CUDA Graph, FP16, profiles and optional overlapped tiling.

## Phase 5 — quality/color

- [ ] Implement cut detection and disable cross-cut interpolation.
- [ ] Add conservative policies for duplicate frames, fast motion and abnormal output.
- [ ] Implement color-managed SDR/HDR and 10-bit conversions.
- [ ] Add animation/text/HUD temporal QA and seam/flicker regression fixtures.
- [ ] Publish objective and blind-review benchmark protocol.

## Phase 6 — desktop

- [ ] Build Home/import and interpolation settings workflows over application services.
- [ ] Implement persistent queue, progress, retry, pause and cancellation.
- [ ] Add comparison player, split mode and frame stepping.
- [ ] Add model center, profiles and expert controls.
- [ ] Localize zh-CN, zh-TW, en and ja; verify accessibility and keyboard use.

## Phase 7 — reliability/pro

- [ ] Design segment-boundary checkpoint format and safe output stitching.
- [ ] Test restart recovery, cancellation, retries, disk-full, OOM and corrupted input.
- [ ] Complete CLI JSON progress, profiles, batch jobs and diagnostics reports.
- [ ] Add advanced encoding controls and quality/performance reports.
- [ ] Validate multi-hour jobs and resume semantics.

## Phase 8 — release

- [ ] Complete dependency/model/runtime license and notice audit.
- [ ] Create signed/unsigned Windows installer and portable builds; verify install/uninstall.
- [ ] Add SBOM, checksums, security review, versioned release notes and support matrix.
- [ ] Add update integrity checks and rollback behavior.
- [ ] Run full P0 acceptance and publish maintenance/release process.

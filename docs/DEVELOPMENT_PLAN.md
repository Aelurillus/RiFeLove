# Executable development plan

Status as of 2026-10-17. This task list expands the Master Plan into reviewable work. It is intentionally not a declaration that the final product is complete.

## Phase 1 — foundation (active)

- [x] Create C++20/CMake/Ninja layout and reproducible configure presets.
- [x] Add exact rational values and a timestamp-based output timeline planner.
- [x] Add SQLite task schema, guarded state transitions and crash-state recovery.
- [x] Add CLI entry points for timeline plans and persisted tasks.
- [x] Add unit tests for arithmetic, VFR/non-integer timeline mapping and task transitions.
- [ ] Add structured logging/configuration, service/backend interfaces and Qt app shell.
- [ ] Add GitHub Actions Windows build/test job and development/build documentation.
- [ ] Validate MSVC + Qt + vcpkg release-toolchain configuration.

## Phase 2 — media engine

- [ ] Probe/demux streams and retain rational stream time bases and metadata.
- [ ] Decode reordered frames and recover PTS including VFR.
- [ ] Add explicit color/pixel-format and HDR metadata handling.
- [ ] Encode and mux output with stream copy for compatible audio/subtitle tracks.
- [ ] Validate duration, frame count, PTS monotonicity, audio sync and container integrity.
- [ ] Add licensed fixture corpus and corrupt/disk-full integration tests.

## Phase 3 — interpolation

- [ ] Audit model code/weight provenance and license; select candidate RIFE models.
- [ ] Define async interpolation/backend contracts, cancellation and reusable frame buffers.
- [ ] Build PyTorch reference exporter and numerical comparison harness.
- [ ] Integrate ONNX Runtime CUDA and validate supported model operators/shapes.
- [ ] Integrate TensorRT backend and version/GPU keyed engine cache.
- [ ] Register signed/checksummed model manifests with capability metadata.
- [ ] Pass real end-to-end video interpolation and forward quality tests.

## Phase 4 — GPU throughput

- [ ] Probe GPU/driver/compute and codec capabilities at runtime.
- [ ] Add NVDEC/NVENC selection with tested fallback paths.
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

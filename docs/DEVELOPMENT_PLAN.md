# Executable development plan

Status as of 2026-10-25. This task list expands the Master Plan into reviewable work. It is intentionally not a declaration that the final product is complete.

Current execution mode: local project, local Git, local builds/tests and local reports. Hosted CI, remote repository setup and publishing are deferred until requested. The final product scope remains the Master Plan baseline.

## Phase 1 — foundation (active)

- [x] Create C++20/CMake/Ninja layout and reproducible configure presets.
- [x] Add exact rational values and a timestamp-based output timeline planner.
- [x] Add SQLite task schema, guarded state transitions and crash-state recovery.
- [x] Add CLI entry points for timeline plans and persisted tasks.
- [x] Add unit tests for arithmetic, VFR/non-integer timeline mapping and task transitions.
- [ ] Add structured logging/configuration, service/backend interfaces and Qt app shell.
  - Qt 6.12 / MinGW 15.1 Studio builds locally, with a background processing service and real QML workflow tests; complete queue/profile/model services, configuration and logging remain open.
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
  - First real AI pipeline now performs full output decode/boundary validation before publication; synthetic container/audio cases pass. Full corpus remains open.
- [x] Add synthetic media integration tests for 10-bit/PQ, tracks/chapters, VFR/B-frames, packet hashes/timing, Unicode paths, corrupt files and output protection.
- [ ] Add the complete licensed fixture corpus, long-video and disk-full fault-injection suite.

## Phase 3 — interpolation

- [x] Verify the author's MIT scope statement and pin/acquire RIFE 4.25 network, checkpoint, warp source and license files.
- [ ] Complete the multi-model provenance audit and quality/profile selection.
- [x] Define the first async interpolation/backend contract with owned GPU inputs, bounded queue, cancellation and exclusive reusable outputs; 36 native reference/contract checks pass (full concurrency/fault stress pending).
- [x] Build PyTorch reference exporter and numerical comparison harness; separately record strict-gate failure and SDR8 numerical qualification.
- [x] Integrate ONNX Runtime CUDA and validate supported model operators/shapes.
  - RIFE 4.25 native execution passes six small-profile SDR8 comparisons with CUDA Conv/ConvTranspose/GridSample placement; larger profiles/visual validation remain open.
- [x] Integrate TensorRT backend and version/GPU/model/options keyed engine cache.
  - ONNX Runtime 1.22 + TensorRT 10.9.0.34 + CUDA 12.8 passed six FP32 SDR8 reference cases on GTX 1660 SUPER; profiling confirms a fused TensorRT kernel ran. A tagged synthetic 720p BT.709 RIFE run completed 30 to 60 FPS with 24 AI frames, preserved FLAC audio and 0.5 s duration. Real-content quality, cache/performance matrix and FP16 remain open.
- [x] Register checksum-verifiable model manifests with capability and license metadata.
  - RIFE pack schema v2 binds the ONNX graph, license, upstream source manifest and validation report by SHA-256. Loading cross-checks tensor capabilities, the numerical profile, declared backends and all passed report cases; the offline Model Center shows the license, provenance declaration and qualification. Legacy v1 packs remain readable with an explicit sidecar-verification warning. The hashes establish pack consistency, not author authenticity; a trusted catalog and multiple qualified model profiles remain open.
- [x] Pass first real end-to-end SDR8 video interpolation and independent original-PyTorch video numerical tests.
- [ ] Complete full-resolution, real-content and perceptual/temporal quality acceptance.

## Phase 4 — GPU throughput

- [x] Add native CUDA device/context/stream/buffer primitives with actual local GPU transfer, budget and lifetime tests.
- [ ] Probe GPU/driver/compute and codec capabilities at runtime.
  - Studio inspects CUDA device information and opens encoder contexts using the selected source profile for NVENC, software H.264, HEVC and AV1. It warns before processing when automatic encoding will fall back and disables a selected codec that fails its preflight. `media-capabilities INPUT [--fps RATE]` reports the same initialization checks. This does not prove encoded-frame output or NVDEC support.
- [ ] Add NVDEC/NVENC selection with tested fallback paths.
  - Auto encoding retries software H.264 when this workstation's NVENC open returns `Function not implemented`; a synthetic 720p TensorRT-to-output run validates the H.264 output and retained audio, and the updated Studio explains that processing continues. Successful NVENC hardware encoding and NVDEC remain unqualified. Local driver 560.94 exposes NVENC API 12.2; this FFmpeg build requires API 13.0 / driver 570.0 or newer.
- [ ] Add bounded VRAM pool, CUDA streams and safe pipeline overlap.
- [ ] Benchmark cold/warm inference and end-to-end throughput by GPU/resolution.
- [ ] Evaluate CUDA Graph, FP16, profiles and optional overlapped tiling.

## Phase 5 — quality/color

- [x] Implement first cut detection and disable cross-cut interpolation, with synthetic hard-cut regression.
- [x] Add first conservative duplicate/abnormal-pair hold policies.
- [ ] Tune and qualify protection on real content, occlusion/fast motion and abnormal model output.
- [ ] Implement color-managed SDR/HDR and 10-bit conversions.
  - Tagged BT.470BG/SMPTE 170M 8-bit SDR now uses explicit BT.601-family YUV coefficients and preserves source primaries/transfer/matrix/range through an actual RIFE encode/decode check; HDR/10-bit and gamut conversion remain open.
- [ ] Add animation/text/HUD temporal QA and seam/flicker regression fixtures.
- [ ] Publish objective and blind-review benchmark protocol.

## Phase 6 — desktop

- [x] Build first single-file picker/drop, media/GPU inspection and interpolation settings workflow over an application service.
- [ ] Complete Home, qualified quality profiles and remaining desktop preferences/workspace behavior.
  - The Studio now has a redesigned dark Material workspace, simplified first-run controls and local default settings. Recursive folder/multi-file import, duplicate elimination, unsupported-color reporting, collision-safe output naming, persistent local defaults and isolated data-directory selection run through the desktop workflow; quality profiles and broader workspace behavior remain open.
- [x] Validate real QML start-to-output, GUI event-loop responsiveness, no-overwrite handling and cooperative cancellation with native RIFE.
- [x] Add offline Model Center local registration, manifest/license/ONNX hash verification, default-model selection and persisted catalog revalidation.
- [x] Complete acceptance for the persistent desktop queue, progress, retry, pause and cancellation.
  - The Qt page is connected to the SQLite service with virtualized task rows, sequential execution, pause/resume, stop, per-task cancel/retry and interrupted restart-from-zero controls. Actual GPU workflow checks recursive batch import, same-name output collision handling, HDR rejection, pause/resume, deterministic output failure/retry, queued cancellation, successful output/audio and preference reload. Full history and safe segment recovery remain open.
- [x] Show estimated output frame totals and remaining time in Studio and persistent queue rows.
  - Estimates are derived from the probed video duration and exact target-rate rational, are retained with task configuration, and remain explicitly approximate until the pipeline verifies the decoded end boundary.
- [x] Add a local comparison player with split view, synchronized playback and output-frame-rate time stepping.
  - Desktop workflow verifies source/result loading, timestamp stepping, playback synchronization within the test tolerance and pause. Exact decoded-frame landing and artifact navigation remain open.
- [ ] Complete model lifecycle with provenance-rich model profiles, qualified presets and expert controls.
  - The desktop Model Center verifies locally installed packs, lists SHA-256 and selects a persistent default without network access. The local workstation bundle now includes one qualified RIFE 4.25 model and starter settings; model downloads, multiple qualified model families, presets and expert controls remain open.
- [ ] Localize zh-CN, zh-TW, en and ja; verify accessibility and keyboard use.

## Phase 7 — reliability/pro

- [ ] Design segment-boundary checkpoint format and safe output stitching.
- [ ] Test restart recovery, cancellation, retries, disk-full, OOM and corrupted input.
  - Actual process termination after flushed encoded output, owner/hard-link exclusion, interruption detection, new-output restart-from-zero, pause/cancel/retry and changed-source rejection pass locally. Segment resume and full fault matrix remain open.
- [ ] Complete CLI JSON progress, profiles, batch jobs and diagnostics reports.
  - Persistent queue add/import/export/list/run/retry/cancel/restart and JSON results are implemented; profiles and cross-process live control remain open.
- [ ] Add advanced encoding controls and quality/performance reports.
  - Studio and Queue now expose bitrate/GOP, HEVC constant QP and AV1 CRF; controls are validated against the selected encoder, persisted in local preferences and included in queue task JSON. Reports, H.264 quality-mode controls, 10-bit/HDR encoding and full codec matrix remain open.
- [ ] Validate multi-hour jobs and resume semantics.

## Phase 8 — release

- [ ] Complete dependency/model/runtime license and notice audit.
- [ ] Create signed/unsigned Windows installer and portable builds; verify install/uninstall.
  - A workstation-only runnable folder is generated by `packaging/windows/package-local.ps1`; it uses an installed CUDA/cuDNN runtime through a local directory link and is not the final installer or portable build.
- [ ] Add SBOM, checksums, security review, versioned release notes and support matrix.
- [ ] Add update integrity checks and rollback behavior.
- [ ] Run full P0 acceptance and publish maintenance/release process.

# Executable development plan

Status as of 2026-10-25. This task list expands the Master Plan into reviewable work. It is intentionally not a declaration that the final product is complete.

Current execution mode: local project, local Git, local builds/tests and local reports. Hosted CI, remote repository setup and publishing are deferred until requested. The final product scope remains the Master Plan baseline.

## Phase 1 — foundation (active)

- [x] Create C++20/CMake/Ninja layout and reproducible configure presets.
- [x] Add exact rational values and a timestamp-based output timeline planner.
- [x] Add SQLite task schema, guarded state transitions and crash-state recovery.
- [x] Add CLI entry points for timeline plans and persisted tasks.
- [x] Add unit tests for arithmetic, VFR/non-integer timeline mapping and task transitions.
- [x] Add structured local logging for desktop, CLI, queue and processing pipeline.
  - spdlog writes timestamped JSONL records to a bounded rotating file; Qt messages, encoder fallback, completion and failure events are covered. The desktop smoke test parses the generated local log and unit tests cover JSON escaping and file rotation.
- [ ] Complete configuration/profile and service/backend interface coverage.
  - Qt 6.12 / MinGW 15.1 Studio builds locally, with a background processing service and real QML workflow tests; complete queue/profile/model services and configuration remain open.
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
- [x] Recover missing PTS only from declared stream start or prior positive frame duration; keep unanchored gaps explicit.
  - Decoder repairs an absent frame PTS only from a declared stream start or the preceding frame's positive decoded duration, never from average FPS alone; media-scan, interpolation results and reports expose the repair count. Unit coverage confirms unanchored gaps remain unresolved instead of guessing from average FPS.
- [x] Preserve/test rotation, reflection, mastering-display and content-light metadata through supported media operations.
  - `media-probe` exposes the exact nine-coefficient display matrix alongside FFmpeg's rotation angle. Horizontal/vertical reflection combined with rotation survives remux and software H.264 transcode to MP4, MOV and MKV. A generated 10-bit PQ fixture combines mastering-display metadata, content-light levels, horizontal reflection and rotation; the probe, MKV remux and AV1 MP4/MKV transcodes preserve the metadata and matrix against ffprobe's semantic values.
- [ ] Expand metadata fixtures across codecs and containers; validate broader codec/container coverage and real HDR media.
- [ ] Add explicit color/pixel-format and HDR metadata handling.
- [x] Add the first explicit tagged BT.709 8-bit SDR conversion to normalized RGB, replicated padding and restored crop; reject unknown/HDR/higher-depth inputs in this path.
- [x] Add a model-independent CPU-frame encoder/mux writer and transcode path with compatible original audio/subtitle stream copy.
- [x] Validate software H.264/HEVC/AV1, FFV1 lossless/HDR, encoder B-frame drain, VFR and exact 60000/1001 scheduling across 1000 generated frames.
- [ ] Validate duration, frame count, PTS monotonicity, audio sync and container integrity.
  - First real AI pipeline now performs full output decode/boundary validation before publication; synthetic container/audio cases pass. Full corpus remains open.
- [x] Add synthetic media integration tests for 10-bit/PQ, tracks/chapters, VFR/B-frames, packet hashes/timing, Unicode paths, corrupt files and output protection.
- [ ] Add the complete licensed fixture corpus, long-video and disk-full fault-injection suite.

## Phase 3 — interpolation

- [x] Verify the author's MIT scope statement and pin/acquire RIFE 4.25 and 4.26 networks, checkpoints, warp source and license files.
- [ ] Complete the multi-model provenance audit and quality/profile selection.
- [x] Define the first async interpolation/backend contract with owned GPU inputs, bounded queue, cancellation and exclusive reusable outputs; 36 native reference/contract checks pass (full concurrency/fault stress pending).
- [x] Build PyTorch reference exporter and numerical comparison harness; separately record strict-gate failure and SDR8 numerical qualification.
- [x] Integrate ONNX Runtime CUDA and validate supported model operators/shapes.
  - RIFE 4.25 and 4.26 native execution pass six small-profile SDR8 comparisons with CUDA Conv/ConvTranspose/GridSample placement; 4.25 also has an end-to-end video and independent PyTorch comparison. Larger profiles and subjective real-content validation remain open.
- [x] Integrate TensorRT backend and version/GPU/model/options keyed engine cache.
  - ONNX Runtime 1.22 + TensorRT 10.9.0.34 + CUDA 12.8 passed six FP32 SDR8 reference cases on GTX 1660 SUPER; profiling confirms a fused TensorRT kernel ran. A tagged synthetic 720p BT.709 RIFE run completed 30 to 60 FPS with 24 AI frames, preserved FLAC audio and 0.5 s duration. Runtime loading now pins TensorRT builder/parser/plugin DLLs from the selected runtime before engine creation; a fresh-cache 320x180 integration run passes even when an older ONNX/TensorRT directory is first on PATH. Real-content quality, cache/performance matrix and FP16 remain open.
- [x] Register checksum-verifiable model manifests with capability and license metadata.
  - RIFE pack schema v2 binds each ONNX graph, license, upstream source manifest and validation report by SHA-256. Loading cross-checks tensor capabilities, the numerical profile, declared backends and all passed report cases; the offline Model Center shows the license, provenance declaration and qualification. The local bundle registers 4.25 and 4.26 with 4.25 selected by default. Legacy v1 packs remain readable with an explicit sidecar-verification warning. The hashes establish pack consistency, not author authenticity; a trusted catalog and quality-ranked profiles remain open.
- [x] Pass first real end-to-end SDR8 video interpolation and independent original-PyTorch video numerical tests.
- [ ] Complete full-resolution, real-content and perceptual/temporal quality acceptance.

## Phase 4 — GPU throughput

- [x] Add native CUDA device/context/stream/buffer primitives with actual local GPU transfer, budget and lifetime tests.
- [ ] Probe GPU/driver/compute and codec capabilities at runtime.
  - Studio inspects CUDA device information and opens encoder contexts using the selected source profile for NVENC, software H.264, HEVC and AV1. Before model loading it rechecks the selected encoder at the requested output frame rate, time base, bitrate, GOP, pixel format and options; automatic mode chooses a software encoder only after that profile opens. `media-capabilities INPUT [--fps RATE]` reports context initialization. This does not prove encoded-frame output or NVDEC support.
- [ ] Add NVDEC/NVENC selection with tested fallback paths.
  - Auto encoding uses a source-compatible software encoder when this workstation's NVENC open returns `Function not implemented`; 8-bit SDR selects software H.264, while a synthetic 10-bit SDR RIFE run selects AV1 to preserve pixel format. The 720p TensorRT-to-output run validates H.264 output and retained audio, and Studio reports CPU encoding separately from CUDA interpolation while showing determinate frame progress. Successful NVENC hardware encoding and NVDEC remain unqualified. Local driver 560.94 exposes NVENC API 12.2; this FFmpeg build requires API 13.0 / driver 570.0 or newer.
- [ ] Add bounded VRAM pool, CUDA streams and safe pipeline overlap.
  - The sequential RIFE path now reuses up to two completed source-frame buffers and releases idle cache entries when allocation pressure requires space; buffers still referenced by an inference request are never recycled. The allocator regression passes on the local GTX 1660 SUPER; end-to-end pipeline profiling, throughput benchmarking and stage overlap remain open.
- [ ] Benchmark cold/warm inference and end-to-end throughput by GPU/resolution.
  - [x] Add a local-only benchmark runner that repeats CLI jobs, excludes warmups from aggregates, records model/GPU/encoder/backend/fallback and stage timing data, hashes source files, and independently checks output frame counts with FFprobe.
  - Studio now reports cumulative RIFE wait, frame encoding, source decode, preparation/transfers/color restoration and final validation time; JSON result/report includes the same stage breakdown. Controlled measurements across GPU/resolution/content combinations and attribution under the full test matrix remain open.
- [ ] Evaluate CUDA Graph, FP16, profiles and optional overlapped tiling.

## Phase 5 — quality/color

- [x] Implement first cut detection and disable cross-cut interpolation, with synthetic hard-cut regression.
- [x] Add first conservative duplicate/abnormal-pair hold policies.
- [ ] Tune and qualify protection on real content, occlusion/fast motion and abnormal model output.
- [ ] Complete color-managed SDR/HDR, gamut conversion and 10-bit visual qualification.
  - Tagged BT.709/BT.601-family 8/10-bit SDR conversion now uses explicit matrix/range handling, maps source profiles into the selected model contract and preserves source bit depth when the chosen encoder accepts it. 10-bit tests currently cover synthetic CPU color conversion; HDR and real-content 10-bit quality remain open.
- [ ] Add animation/text/HUD temporal QA and seam/flicker regression fixtures.
- [ ] Publish objective and blind-review benchmark protocol.

## Phase 6 — desktop

- [x] Build first single-file picker/drop, media/GPU inspection and interpolation settings workflow over an application service.
- [ ] Complete Home, qualified quality profiles and remaining desktop preferences/workspace behavior.
  - Home now opens first and shows the detected local NVIDIA GPU/VRAM, selected local model, persistent queue counts, recent tasks and the tested encoder fallback policy; Studio/Queue/Model Center shortcuts are connected and the desktop workflow captures the Home view. The Studio also has a redesigned dark Material workspace, simplified first-run controls and local defaults. Recursive folder/multi-file import, duplicate elimination, unsupported-color reporting, collision-safe naming and isolated data-directory selection run through the desktop workflow. Qualified quality profiles, broader keyboard/accessibility coverage and remaining workspace behavior stay open.
- [x] Validate real QML start-to-output, GUI event-loop responsiveness, no-overwrite handling and cooperative cancellation with native RIFE.
- [x] Add offline Model Center local registration, manifest/license/ONNX hash verification, default-model selection and persisted catalog revalidation.
- [x] Gate Studio processing on verification of the exact selected local model and its declared input profile.
  - Model verification starts even when the persistent queue becomes ready before QML loads; selecting a different unverified folder disables processing for SDR8 and SDR10 inputs. Verified legacy packs retain their explicit SDR8 compatibility rule. Missing model-file errors include the local path for diagnosis.
- [x] Complete acceptance for the persistent desktop queue, progress, retry, pause and cancellation.
  - The Qt page is connected to the SQLite service with virtualized task rows, sequential execution, pause/resume, stop, per-task cancel/retry, checkpoint-resume and restart-from-zero controls. Actual GPU workflow checks recursive batch import, same-name output collision handling, HDR rejection, pause/resume, deterministic output failure/retry, queued cancellation, successful output/audio and preference reload. Full history and broader checkpoint fault coverage remain open.
- [x] Show estimated output frame totals and remaining time in Studio and persistent queue rows.
  - Estimates are derived from the probed video duration and exact target-rate rational, are retained with task configuration, and remain explicitly approximate until the pipeline verifies the decoded end boundary.
- [x] Add a local comparison player with split view, synchronized playback and output-frame-rate time stepping.
  - Desktop workflow verifies source/result loading, timestamp stepping, playback synchronization within the test tolerance and pause. The comparison page shows FFmpeg-decoded still frames while paused, steps by actual previous/next presentation timestamps for CFR and VFR clips, and opens the local source/result containing folders. The Qt workflow test verifies both folder targets through an injected opener and rejects non-local URLs; HDR/unknown-color exact preview remains open.
- [ ] Complete model lifecycle with provenance-rich model profiles, qualified presets and expert controls.
  - The desktop Model Center verifies locally installed packs, lists SHA-256 and selects a persistent default without network access. The local workstation bundle includes two pinned RIFE packs, starter settings and source-compatible software fallback messaging when NVENC fails preflight; model downloads, quality-ranked profiles and several expert controls remain open.
- [x] Add offline zh-CN, zh-TW, en and ja desktop catalogs with a locally persisted language selector; verify embedded catalog loading in the real QML startup smoke test.
- [ ] Complete broader keyboard and accessibility review.

## Phase 7 — reliability/pro

- [x] Define the segment checkpoint manifest, contiguous CFR frame ranges, commit ordering, identity checks and safe assembly contract; add immutable manifest and contiguous segment journal storage in SQLite.
- [x] Add a video-only writer mode that enforces an independently decodable first packet; test audio omission, nonzero global PTS origin and exact frame boundaries.
- [x] Verify finalized segment stream/timestamp/color contracts and stream-copy assemble verified segments with original auxiliary streams, chapters and no-replace publication.
- [x] Add task-scoped artifact paths, SHA-256 journaling and full file/media re-verification for recorded segments.
- [x] Integrate segment creation/hashing with persistent queue work, verify/reuse committed segments after process interruption, decode from source start to rebuild RIFE context, assemble/publish the result, and expose desktop/CLI checkpoint resume.
  - [x] On explicit resume, remove unjournaled segment files and partial writers from the task-owned segment directory only after verifying the immutable manifest and committed segments; preserve unrelated files.
- [x] Persist the validated final output's SHA-256, byte count and result before publication; on restart, adopt only the unchanged published file and reject modified or unproven outputs.
  - Queue startup now reconciles that boundary automatically while holding a read lease over the output: exact size/hash plus a structurally valid result/frame-count record are required before the task becomes completed. Crash integration verifies automatic adoption, and separately rejects altered bytes and corrupted result data without changing the output.
- [ ] Test restart recovery, cancellation, retries, disk-full, OOM and corrupted input.
  - The real queue integration test kills a worker with one verified segment and a live partial, then resumes with exact 66-frame output, bit-identical audio and unchanged source. A second interruption after all 64 output frames are committed resumes directly to assembly. A third interruption after final-output publication adopts the byte-identical file and rejects a deliberately altered copy. Stale task-owned partial/orphan segments are cleaned on explicit resume, while unrelated task-directory files survive. Disk-full, OOM, corrupt-input fault injection and multi-hour tests remain open.
- [ ] Complete CLI JSON progress, profiles, batch jobs and diagnostics reports.
  - Persistent queue add/import/export/list/run/retry/cancel/restart and JSON results are implemented; direct interpolation now has an opt-in atomic JSON report for model/GPU identity, encoder fallback, throughput, protection counters and timing validation. Profiles, queue report export and cross-process live control remain open.
- [ ] Add advanced encoding controls and quality/performance reports.
  - Studio and Queue expose bitrate/GOP, HEVC constant QP, AV1 CRF and explicit H.264 NVENC VBR + CQ (0–51); settings are validated and persisted. This workstation's driver cannot initialize NVENC, so actual CQ output remains unverified; automatic mode preflights to compatible software H.264 while RIFE remains on CUDA. A local-only SDR reference evaluator checks target timestamps and reports PSNR/SSIM by source-coincident and non-source sample. An aligned 320x180 synthetic 30-to-60 run produced 120 frames (59 RIFE, 60 original, one held); exact source frames matched the reference and the 60 non-source samples averaged 28.67 dB PSNR / 0.945 SSIM. These are synthetic pipeline checks, not real-content quality claims. Direct interpolation JSON and Studio include stage timing data. Real-content blind review, HDR quality reports and the supported-device codec matrix remain open.
- [ ] Validate multi-hour jobs and resume semantics.

## Phase 8 — release

- [ ] Complete dependency/model/runtime license and notice audit.
- [ ] Create signed/unsigned Windows installer and portable builds; verify install/uninstall.
  - A workstation-only runnable folder is generated by `packaging/windows/package-local.ps1`; it uses this machine's CUDA/cuDNN/TensorRT runtime through a local directory link and is not a cross-machine portable build or final installer.
- [ ] Add SBOM, checksums, security review, versioned release notes and support matrix.
- [ ] Add update integrity checks and rollback behavior.
- [ ] Run full P0 acceptance and publish maintenance/release process.

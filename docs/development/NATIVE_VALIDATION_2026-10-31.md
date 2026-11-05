# Local native validation — 2026-10-31

These results describe one development machine, not release qualification. The
GPU was an NVIDIA GeForce GTX 1660 SUPER; the native build used the x86asm
FFmpeg 8.1.3 SDK and the workstation's local ONNX Runtime/TensorRT runtime.
The tested `RifeLoveNative.exe` SHA-256 was
`38e5a55eeaf0f5034fea3ed83f1004d52baa8d37527a11c1af7300c537a6ca81`.

- The Release build passed 171 non-GPU CTest cases and eight serial CUDA /
  TensorRT cases. Separate package and FFmpeg SDK selection checks passed,
  including 20 SDK discovery and stale-cache cases.
- A new portable QA package was verified against its immutable-file checksums
  and the build EXE, then launched on a separate Windows desktop with an empty,
  independent data directory. Its actual start button completed three jobs
  from a 192×128, 30 FPS, 0.4-second synthetic FFV1/FLAC source: opt-in
  automatic Gaussian denoising at 60 FPS, 360 FPS interpolation, and optional
  temporal denoising plus ×2 super-resolution and motion blur at 60 FPS.
  Queue records identify TensorRT/CUDA inference and H.264 NVENC for these
  jobs. The source used software decode because FFV1 had no CUDA decoder.
- The three files were independently decoded. The 360 FPS file contained 144
  frames; sorted packet timestamps were strictly increasing and differed by at
  most 0.445 ms from the ideal clock, consistent with Matroska millisecond
  timestamps. All five copied audio packets, their timestamps and hashes
  matched the source. The other two outputs contained 24 frames and retained
  the same decoded audio hash. All three had a 0.4-second container duration.
- A separate synthetic RIFE plus FastDVDnet CUDA interruption/recovery test
  exercised the new source-bound checkpoint identity. It reused four of 24
  output frames from two committed segments; decoded video pixels, timestamps
  and PCM audio matched the uninterrupted reference, and the source was
  unchanged.
- The prior no-x86asm SDK could corrupt chroma during some 4:4:4-to-4:2:0 or
  spatial conversions. The writer now requests accurate rounding, and a
  decoded-pixel regression plus the x86asm SDK were checked independently.

Automatic denoising can remove intentional film grain and remains disabled by
default. These short synthetic tests do not establish perceptual quality on
real footage, long-video stability, HDR correctness, multi-monitor DPI
behavior, compatibility with other GPUs, or redistribution rights. Local
models and runtimes were not uploaded to the repository.

## Follow-up: output validation and publication recovery

A later four-second synthetic VFR fixture exposed a defect in the preceding
preview: while validating a result of at least 120 frames, the progress callback
could report the partial verification count together with the full processed
count. Depending on callback timing, the queue rejected the inconsistent
counters and reported failure before publishing. The validator now keeps the
complete output count and reports checked frames separately, without weakening
timestamp or media validation.

The subsequent local EXE SHA-256 was
`116a0014f4c3f2b0597442ae9bd0dae2a6a910a7e6e8439d749eb48b3f5fee87`.
Its Release build passed 179 non-GPU tests and eight serial CUDA/TensorRT
tests. A separate portable QA package ran the actual desktop workflow on the
VFR fixture: 144 irregularly timed source frames became 240 frames at 60 FPS,
using TensorRT CUDA and H.264 NVENC with 95 RIFE inference calls. Independent
decoding succeeded; maximum output PTS error against the 60 FPS clock was
0.334 ms, both audio streams and the subtitle retained their packet hashes and
timestamps, the two chapters were unchanged, and the source hash was unchanged.
The resulting local evidence and reusable developer-only driver are
`work/verification/native-r13-vfr-multitrack-final-20261031/result.json` and
`tests/integration/native_vfr_package_test.py` respectively.

In a separate fresh portable QA package, the same EXE generated a two-second
192×128, 30-to-60 FPS synthetic comparison clip and opened its comparison
window. A cross-process control test checked playback advancement, a 300 ms
pause, forward/backward stepping, exact half-second seek and playback resume.
The output's independently probed frame list confirmed that the observed
133 ms and 150 ms steps were adjacent actual presentation timestamps. This
checks the muted transport and its reported timestamps, not framebuffer
appearance, audio playback or high-resolution/long-video preview performance.

Separate real SQLite fault-injection tests exercise a verified, published
file when completion or final-progress persistence fails, including recovery,
conflicting output and external modification. They substitute a small CPU
engine fixture; they are not long-video or real-GPU failure qualification.
The prior preview should not be used for formal processing where this
validation-progress error may occur. These checks remain synthetic, local,
and do not establish stable-release acceptance.

## Follow-up: r15 native compression controls and package contracts

The r15 native EXE SHA-256 is
`c906412ff8c66c851de039c446f6dfdddba3510d27827b98e2a7e0989fd60c62`.
The Release build completed and all **200 CTest cases passed**, with no skipped
cases. This includes the CPU-only model-reuse and Normal/Portable data-mode
packaging contracts. The real r14 four-model reuse check passed 60 assertions;
clean-checkout fixtures passed 57 without local weights or pre-existing output.

Independent package verification checked every immutable-file checksum and
matched both r15 EXEs to the build: 112 files in the Normal package, 113 in the
Portable QA package. Both initially contained empty package-local data; Normal
mode continues to resolve user data through `%LOCALAPPDATA%\RifeLove`, while
Portable mode requires its explicit marker. The older r14 package was preserved.

The actual Portable QA EXE was driven on a separate desktop with an isolated,
initially empty queue. The independent compression model and strength controls
started off, then selected STDF at 50%, with denoise and super-resolution off.
It completed a 192x128, 30-to-60 FPS, 0.4-second synthetic FFV1/FLAC fixture.
The read-only queue check confirmed schema 10, pinned STDF descriptor identity,
software source decode, real RIFE inference and validated completed counters.
The output used H.264 NVENC and contained 24 independently decoded frames,
BT.709 limited-range SDR8, a 0.4-second duration and maximum PTS error of
0.334 ms. All five audio packet hashes and timestamps matched the source.

The first fixture was refused before processing because its container lacked
color primaries and transfer tags. A separate lossless fixture with all four
color properties explicitly set passed; the original was preserved unchanged.
Evidence is local at
`work/verification/native-r15-compression-20261031/result.json`. No videos,
weights, runtime libraries or user queues are part of the source commit.

This remains a small-frame functional evaluation, limited to qualified CUDA,
tagged BT.709 limited-range `yuv420p` inputs no larger than 384x256. It does not
qualify full-resolution temporal restoration, real-footage perceptual quality,
long-video reliability or a stable release. No usable new rendered screenshot
was captured, so rendered visual acceptance is still outstanding.

## Follow-up: r16 explicit bounded UHD STDF evaluation

The r16 Normal and isolated Portable QA EXEs have SHA-256
`d9f7130ceb7f5173b3688daceca511de083bee3124fde87b222879f8acb4f868`.
They contain the same four local model families as r15. Independent verification
matched all 112 immutable Normal-package files and 113 Portable-package files;
the Normal package has empty package-local data and the Portable QA package now
contains only its synthetic completed task. Model/runtime resources are verified
NTFS hard links rather than fresh multi-GiB copies. Compression restoration and
the separate high-resolution tiled switch both default to off.

The new switch selects the fixed
`stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1` policy; it does not raise
the existing whole-frame graph limit. Schema 11 stores an explicit boolean and
complete tile geometry/padding/size policy. Schema 1-10 remains full-frame;
full-frame/tiled checkpoints cannot be interchanged. The source identity
inventory now explicitly includes both new output-affecting adapter headers.

Release configure/build passed, followed by 211/211 CTest tests, 44 Python
model-tool contracts and both the reduced/native-processing control harnesses.
Seven serial real CUDA checker cases passed every-pixel independent readback,
including 1080p and landscape/portrait UHD, odd dimensions, bounded graph shapes,
center chroma ownership and exact rational metadata assertions. The detailed
reference and memory/performance limits are in [STDF_R3_TILING.md](STDF_R3_TILING.md).

The actual Portable QA EXE was driven through its HWND model, strength and tiled
controls, starting explicitly from off and selecting STDF 50% with the tiled
checkbox checked. It processed a 1920x1080, 30 FPS, 0.4-second FFV1/FLAC fixture
through STDF source-rate restoration, RIFE TensorRT/CUDA and H.264 NVENC. The
read-only queue contains a completed schema 11 task, the exact tiled policy,
12 source frames, 11 real RIFE inference calls and 24 validated output frames.
Application elapsed time was 50.970 seconds, including 41.747 seconds recorded
for restoration; this is slow experimental processing, not real-time video.

Independent FFprobe/FFmpeg checks confirmed 24 frames, 60 FPS, 1920x1080,
0.4 seconds, BT.709 TV-range yuv420p, maximum timestamp error 0.334 ms, all five
original FLAC packet hashes/timestamps and error-free full video/audio decode.
Both source fixture hashes remain unchanged. Encoder-level color flags alone
had omitted primaries/transfer on the first synthetic input; a separate lossless
frame-tagged fixture was explicitly probed before the successful GUI run.
Evidence: `work/verification/native-r16-tiled-compression-20261031/result.json`.

The in-process native renderer/control harness captured the full client, narrow
layout and 480-DIP top/bottom views. An initial geometry test used a scroll offset
left by prior preset interactions; it now explicitly returns to the top before
checking painted label/field coordinates. The corrected native checks pass.
Images stay at `work/verification/native-r16-ui-20261031/client-verified*.png`.
These are source-harness rendering evidence, not a packaged-EXE framebuffer
capture or broad per-monitor-DPI/aesthetic acceptance.

This is an opt-in local evaluation package. Real-content perceptual evaluation,
adaptive compression/detail controls, long videos, HDR, the hardware matrix,
installer and redistribution acceptance remain open. No model, runtime, queue,
video, generated image or cloud credential is part of the source commit.

## Follow-up: r17 boundary correction and packaged recheck

Final core review identified undefined intermediate pointer arithmetic when a
short tile was cropped at a large global X coordinate. For example, a 3840x8
source can produce a global-X intermediate pointer outside the prediction
array even though subtracting the tile origin would bring it back inside.
The adapter now computes the entire tile-local integer offset first and forms
the pointer once. CPU packing/cropping and exact-coverage contracts additionally
exercise 3840x8, 3839x7 and 8x3840; this is a static correctness fix, not a claim
that a crash was observed.

The rebuilt r17 EXE SHA-256 is
`fe74d2c50c0492f21f55a2b8d1d1bcae27889dc21af85ae6f1e0a14c5630e0a7`.
Release configure/build, all 211 CTest cases and all 44 Python model-tool tests
passed again. The same seven serial native CUDA cases, including both UHD
orientations, passed independent all-byte readback with maximum Y8 difference
at most one. Fresh evidence is under
`work/verification/stdf-tiled-boundary-20261031/`; the earlier measurements and
evidence remain historical rather than being overwritten.

Normal and Portable QA r17 packages independently verified 112 and 113 immutable
file hashes respectively. Existing verified model/runtime files were reused
through NTFS hard links; neither old packages nor the running r14 process were
modified. The Normal package has empty package-local data, while the separate
Portable QA package now contains its completed synthetic task.

The actual packaged QA EXE repeated the explicitly selected STDF 50% tiled,
1080p, 30-to-60 FPS, 0.4-second workflow through RIFE TensorRT/CUDA and H.264
NVENC. Independent decoding confirmed 24 frames, correct color/timestamps and
all five unchanged FLAC packet hashes/timestamps. Application elapsed time was
51.252 seconds, including 42.171 seconds of restoration. Both inputs remain
unchanged. Evidence is at
`work/verification/native-r17-tiled-compression-20261031/result.json`.
The experimental/default-off and incomplete product-acceptance limits above
still apply.

Only necessary source, tests, build/packaging configuration and documentation
were committed. Preflight found no binary/model/media/database artifacts or
credential patterns among the 39 staged files. Implementation commit
`7bb3e43` passed both GitHub Actions jobs in run `41168197523`; those remote
jobs are CPU/Windows checks, not remote GPU qualification.

## Local storage cleanup evidence

Obsolete generated package files were removed only after checksum checks;
remaining model/runtime copies were deduplicated where verified hard links
were safe. User data, results, checkpoint identities and the runnable current
package were preserved. Three measured cleanup phases increased free space by
33,067,008 + 52,613,120 + 5,064,253,440 bytes, approximately 4.80 GiB total;
parallel builds can affect the live drive-wide balance.

A separate check of 40 database files (25 actual SQLite databases) and 15
checkpoint JSON files found no references to three retired candidate models.
Deletion of their ZIP/weight files was nevertheless rejected by automatic
approval with `blocked by policy`, including a narrower attempt preserving the
ZIPs. Those 136,044,118 bytes remain untouched. Detailed evidence stays local
under `work/verification/cleanup-audit-20261031/`.

A later request to remove obsolete generated directories under `out/` was also
rejected by automatic approval with `blocked by policy` before execution.
No deletion occurred in that attempt, and it was not retried through another
tool or agent. Do not count the candidate-size estimate as reclaimed space;
hard-linked file lengths also do not equal independent physical allocation.

## Processing-enabled native control harness (2026-11-05)

On the merged `9bb3679` source, a fresh native Release configuration enabled
FFmpeg and ONNX CUDA with the local pinned SDKs. The application target built,
but the optional processing-control harness initially failed to compile: it
lacked the JSON include path and still constructed a comparison request using
an obsolete positional initializer. The harness now uses the same JSON and
Windows cryptography dependencies as the desktop target, constructs that
request by named fields, and is registered as `NativeDesktop.ProcessingControls`
when the processing service is built. Its automatic-denoise navigation fixture
opens the quality disclosure before checking the exposed controls and restores
the initial disclosure state afterward.

The full processing-control harness and the CPU-only native control harness
passed via CTest (2/2). A subsequent full build succeeded; the non-hardware
CTest run reported 248 passed and two skipped out of 250. The skipped cases
could not find local RIFE/STDF evaluation packs in this worktree; running those
two checks separately from the existing model-cache checkout passed 2/2.
GPU hardware tests were excluded.
The processing harness also passed its no-argument
local model, settings and comparison-state fixtures. An explicit comparison
run decoded the existing tagged synthetic 192×128, 30/120 fps MP4 pair and
passed paired-time selection, adjacent-frame stepping, slider and EOF checks;
the reflected/offset display-matrix rejection path was exercised as well.
Screenshots from the isolated desktop are local under
`out/ui-readiness-main-20261105/`. They are synthetic control evidence, not
representative visual-quality, GPU throughput, or release-package acceptance.
Windows desktop capture timed out twice during the separate live-window
attempt, so real-display and mixed-DPI visual acceptance remains open.

## Native start-to-output smoke test (2026-11-05)

The processing-enabled control harness built from `b146752` was staged in a
new isolated directory with the existing verified RIFE 4.25 v3 pack and local
GPU runtime. An initial attempt correctly refused to start because the runtime
was not in the application's discovery directory; after staging the runtime,
the harness imported the source, selected 60 FPS and H.264 NVENC, and activated
the native Start control. It reported `completed`, `h264_nvenc` and 120 output
frames. Inference was configured as `auto` (TensorRT preferred); this run did
not independently record which inference provider was actually selected.

The synthetic input was an existing 192×128 BT.709 H.264 clip, 60 frames at
30 FPS over two seconds. The output was 192×128 H.264 `yuv420p` TV-range
BT.709, 120 frames at 60 FPS over two seconds. A separate FFmpeg decode of the
complete output returned no errors. Source SHA-256:
`c3cf55a40b84191283474bab6fd001bd605e6c70b3907d8755c5d598340848b4`;
model ONNX SHA-256:
`8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`;
output SHA-256:
`f2c51160b5bc221e387bf18bab58b2cf00e52a8fd5f2f0160d24dbbd13d53132`.
The machine reported a GTX 1660 SUPER, 6 GiB and driver 560.94. Output, fixture
and an imported-window capture remain local under
`out/native-processing-e2e-20261105/`; they are not release artifacts.

The test drives real native controls on an isolated Windows desktop but uses a
test executable, a synthetic short clip, and locally staged dependencies. It
does not establish perceptual quality, representative throughput, long-video
reliability, interactive desktop accessibility, mixed-DPI behavior, or a clean
machine's packaged installation. Those remain separate release checks.

A second run with the same source and model extended the harness to read the
completed task's persisted result and compare its output-frame count with the
native UI. It recorded `onnxruntime_tensorrt_cuda`, 59 RIFE-generated frames,
59 inference calls, one held frame, and 120 total output frames; the full
output independently decoded without errors. The second output SHA-256 is
`d8678028a4558c933673cc31e7d086010f0c5698020c24d368900706f2473e98`.
The provider label describes the configured ONNX Runtime session, not proof
that every operation ran on TensorRT. Other acceptance limits above still apply.

## RIFE-only packaging candidate (2026-11-05)

The new `-RifeOnly` selection built a fresh full local bundle from the current
native executable and local FFmpeg, inference runtime and RIFE 4.25 v3 pack.
Its manifest listed exactly one model, all restoration paths in defaults were
null, the initial data directory was empty, and Qt/CLI were absent. The full
package build used `-SkipStartupSmoke` to avoid writing to the ordinary Windows
user profile. It was then reused into a fresh portable bundle with an explicit
checksummed portable marker. Independent readback verified all 95 listed
immutable file checksums and confirmed the one-model manifest and null SR path.

The portable executable started with a fresh data directory and created its
own queue database there. The first hidden-window smoke attempt could not use
`CloseMainWindow` to find the hidden window and forcibly stopped that test
process. A second launch located the native window by its class and process ID,
closed it normally with `WM_CLOSE`, and exited with code zero; afterward its
portable data directory contained the queue database. Neither launch is an
interactive end-to-end media test. A previous attempt to reuse an older bundle
was rejected because the new executable's imports differed; a fresh full
package was built rather than bypassing the import check. These local bundles
are not public release artifacts and retain the release and license gates above.

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

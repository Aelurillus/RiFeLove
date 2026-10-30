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

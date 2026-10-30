# Isolated FFmpeg source SDK

This document describes the reproducible FFmpeg SDK used for native Windows validation. It is separate from the normal repository dependency setup and keeps its source extraction, build tree, install tree and logs below `work/`.

The tracked lock file is [ffmpeg-source-sdk.lock.json](../../tools/development/ffmpeg-source-sdk.lock.json). The build entry point is [build-ffmpeg-source.ps1](../../tools/development/build-ffmpeg-source.ps1). The script never changes the system PATH, NVIDIA driver or top-level CMake configuration. It expects locally reviewed archives and fails when a required file or SHA-256 does not match.

## Inputs

The locked inputs are FFmpeg 8.1.3, NVIDIA `nv-codec-headers` n12.2.72.0, NASM 3.02, libaom 3.15.2, OpenH264 2.7.0 and Kvazaar 2.3.2. The codec libraries are consumed from the local prefix at `work/codec-deps/installed/x64-mingw-dynamic`; their source URLs and hashes remain recorded even though this script does not rebuild that prefix.

The intended shell is an isolated MSYS2 UCRT64 environment under `work/msys64`. A normal MSYS shell is rejected by FFmpeg's configure script for native Windows builds. The current compiler is the local MinGW-w64 toolchain; NASM is supplied by the locked local archive and placed in the child process PATH only.

The build uses two local FFmpeg patches:

- `0001-response-files.patch` keeps large Windows shared-library link commands below the `CreateProcess` command-line limit.
- `0002-makedef-mri.patch` lets `compat/windows/makedef` consume response files and uses GNU `ar` MRI input so duplicate object basenames are not silently replaced in the temporary export archive.

The patches are limited to the isolated source build. They do not alter the application pipeline or the existing SDK.

## Rebuild

From PowerShell in the repository root:

```powershell
.\tools\development\build-ffmpeg-source.ps1
```

The default output is `work/codec-ffmpeg-build-x86asm-repro/ffmpeg-install`. To prepare a fresh source tree and validate hashes without compiling:

```powershell
.\tools\development\build-ffmpeg-source.ps1 `
  -BuildRoot work/ffmpeg-source-preflight `
  -SkipBuild
```

The script uses 7-Zip for the two-stage `tar.xz` extraction, applies both patches, runs FFmpeg configure with `--enable-x86asm`, then runs `make` and `make install`. It does not download dependencies or contact a release bucket.

## Configure boundary

The resulting SDK is intended to retain the existing media capabilities:

- CUDA, NVDEC and NVENC integration through n12.2.72.0 headers.
- H.264 NVENC and HEVC NVENC hardware encoding where the installed GPU supports them.
- OpenH264 software H.264, Kvazaar software HEVC and libaom software AV1 fallback.
- FFmpeg shared-library linkage for the native application, rather than an `ffmpeg.exe` subprocess requirement.
- Audio, subtitle, chapter and container handling from the same FFmpeg library set.

This SDK does not qualify every NVIDIA GPU, driver or TensorRT version. The locally tested GTX 1660 SUPER supports H.264 NVENC and HEVC NVENC 8-bit/10-bit. AV1 NVENC is not available on that GPU. H.264 10-bit NVENC is also unavailable there and must use a qualified software path or another supported format.

The baseline SDK at `work/codec-ffmpeg-build/ffmpeg-install` was built with `--disable-x86asm`. The source SDK enables NASM assembly and is an experimental comparison build until the output, native DLL linkage and A/B timing checks finish. A passing encoder registration probe is insufficient: each encoder must write a sample file, and the sample must be independently decoded and inspected.

## Validation procedure

Run the same fixture and settings against both SDK roots. Record the exact input hash, frame count, resolution, output pixel format, encoder options, thread count, warm-up policy and repeated-run count. Measure software H.264, software HEVC, software AV1 and CPU decode separately. Then run the native RIFE/CUDA path using the same small tagged SDR fixture and compare output frame count, duration, timestamps, audio/subtitle streams, codec and decode status.

The native path must load FFmpeg DLLs directly and exercise CUDA/NVDEC decode, RIFE inference and the selected encoder. The standalone `ffmpeg.exe` tests are useful evidence but do not replace this regression. Keep generated media, timings and reports under `work/` or ignored `out/`; do not commit them.

## 2026-10-28 local evidence

The x86asm SDK was built from the locked inputs with `HAVE_X86ASM=yes`, `X86ASM=nasm`, `X86ASMFLAGS=-f win64`, `CONFIG_CUDA=yes`, `CONFIG_NVDEC=yes`, `CONFIG_NVENC=yes`, OpenH264, Kvazaar and libaom enabled. The runtime DLLs were copied into the isolated validation directory only; the baseline SDK was left intact.

Real file writes and independent decode passed for H.264 NVENC and HEVC NVENC (30 frames each), OpenH264 H.264, Kvazaar HEVC and libaom AV1. A 10-bit libaom AV1 output also retained `yuv420p10le` and decoded successfully. An x86asm remux preserved H.264 video, AAC audio and SRT subtitle streams; video, audio and subtitle decode checks passed separately. CUDA hardware decode of the x86asm H.264 file passed with `-hwaccel cuda -hwaccel_output_format cuda`. The native application was rebuilt against the x86asm SDK and passed the six-case ONNX Runtime CUDA validator with 36 contract checks. A direct native RIFE run then produced 8 frames at 8 FPS from the tagged 4-frame fixture, preserved AAC audio, reported CUDA source decode and no inference/encoder fallback, and independently decoded with zero errors. Evidence is retained in `work/native-e2e/x86asm-native-cuda-report.json`, `work/native-e2e/x86asm-native-rife-cuda-openh264-report.json` and the generated media files.

For the CPU A/B, both SDKs encoded the identical 640x360, 120-frame YUV4MPEG fixture three times per software encoder, then the output was probed for 120 frames and decoded. Mean wall time and derived throughput were:

| Path | No x86asm | x86asm | Change |
| --- | ---: | ---: | ---: |
| OpenH264 H.264 | 0.1990 s / 603 fps | 0.2015 s / 596 fps | -1.2% |
| Kvazaar HEVC | 1.0901 s / 110 fps | 1.0581 s / 113 fps | +3.0% |
| libaom AV1 | 4.9371 s / 24.3 fps | 4.8836 s / 24.6 fps | +1.1% |
| H.264 CPU decode | 0.0482 s / 2,490 fps | 0.0358 s / 3,352 fps | +34.6% |

These are small synthetic runs on one workstation, with three encode repeats and five decode repeats. They establish that the x86asm build is functional and give a reproducible comparison, but they do not qualify user-content quality or long-video throughput. The packaged native preview continues to use the already validated no-x86asm SDK until the source build is deliberately promoted after review.

## Licensing and source review

### 2026-10-31 native SDK regression follow-up

The existing x86asm SDK was configured into the separate Release build
`build/native-sdk-regression-x86asm`; the baseline SDK, existing application
builds and user bundles were not replaced. All three CMake cache entries
(`RIFELOVE_FFMPEG_ROOT`, `RIFELOVE_FFMPEG_TOOL`, `RIFELOVE_FFPROBE_TOOL`) point to
the candidate. Copied runtime DLL hashes match the candidate SDK.

The current no-x86asm baseline can silently saturate chroma in its legacy
swscale path when converting 4:4:4 to 4:2:0. The product writer now uses
`SWS_ACCURATE_RND`, matching the existing preview/RIFE converters; a red/green
known-pixel unit regression covers conversion and spatial resizing. This is a
pixel correctness repair, not an encoder fallback or output-count repair.

Additional developer drivers live at
`tools/validation/native_sdk_writer_check.cpp` and
`tools/validation/native_sdk_processing_check.cpp`. They link to the selected
native build's libraries and are not packaged as an end-user CLI. The writer
driver submits known generated frame planes directly to `VideoMuxWriter` and
does not claim to run a super-resolution model. Seven candidate cases passed
independent FFmpeg decode with zero plateau pixel error (excluding the intended
filtered color boundary): planar 4:2:0 unchanged/upscaled, 3840x2160 to
2560x1440 same-layout scaling, NV12 unchanged/downscaled, planar 10-bit
downscaled, and P010 downscaled. This specifically covers the writer resize
stage used after restoration instead of extrapolating from 4:4:4 conversion.

Natural 480x360 12-frame clips encoded through OpenH264, Kvazaar, libaom 8-bit
and libaom 10-bit all passed independent video/audio decode and retained two
AAC tracks. Decoded comparison PSNR was 41.4-43.9 dB against the fixture;
that checks this corruption mode, not RIFE quality. Evidence remains local in
`out/native-sdk-regression-20261031/` (`cpu-codecs.json`, `writer-cpu.json`).
The separate build passed the 53-test focused media/color/queue set. A broader
153-test CPU pass exposed independent in-progress UI and legacy-checkpoint
failures; it is not recorded as a complete green release gate. Existing Oct
28/29 records above remain historical and do not qualify the current source
revision.

Packaging now defaults to the candidate but refuses mismatched build/tool/DLL
identity. It records actual configuration and binary hashes. A real attempt to
reuse the old r11 baseline package with the candidate build was rejected on
`avcodec-62.dll` before creating a staging/output package.

After the GPU slot was released, six-case/36-check real RIFE reference tests
passed for both ONNX CUDA and TensorRT FP32; six GPU memory/lifetime tests also
passed (8 CTest entries total). Natural 12-frame clips encoded with H.264 NVENC,
HEVC NVENC 8-bit and HEVC NVENC 10-bit passed independent decode, with two AAC
tracks preserved. Actual product writer NV12/H.264 and P010/HEVC scaling from
3840x2160 to 2560x1440 also passed; NV12 plateaus were exact, P010 chroma maximum
errors were 3 and 2 ten-bit code values, with no pixels outside the declared
two-eight-bit-level tolerance.

Three real native application-service runs passed: CUDA RIFE with OpenH264,
TensorRT RIFE with NVENC/NV12, and CUDA RIFE with NVENC/P010. Each produced 24
frames at 48 FPS over the original half-second duration, with actual CUDA
hardware source decode and no reported provider/encoder fallback. Independent
FFmpeg decoded every frame and confirmed a maximum 0.5 ms timestamp error,
consistent with the Matroska 1 ms time base. Both copied AAC tracks retained
all 25 packet payload hashes and all original packet PTS in each run. The
ten-bit source's nonzero 42 ms video origin was preserved. First-original-frame
PSNR was 40.3, 57.0 and 53.8 dB respectively; this is a corruption guard, not an
interpolated-frame quality score. Evidence: `gpu-codecs.json`,
`writer-gpu.json`, `real-rife-independent-validation.json`, and the build's
`gpu-reference-regression.xml`. CUDA logged cuDNN convolution algorithm
fallback warnings, so no general performance claim is made from these clips.

`-RefreshFfmpegRuntime` now permits the reviewed same-source configuration
transition without copying the entire inference runtime/model set. It still
checks all source hashes, native/compiler runtime identity, source-lock
identity and DLL dependency imports, then copies only differing FFmpeg files
and regenerates dependency/configuration/hash manifests. The new local
candidate `out/RifeLoveNative-local-20261031-x86asm-sdkcheck` was generated from
r11 with seven new DLLs (33,423,872 bytes), plus the new executable and small
metadata. Unchanged large resources are NTFS hard links; r11 files remain
unchanged. Startup/GUI acceptance of this specific new executable remains
pending, as do long-video, broad GPU/content and redistribution qualification.

Review the version-specific terms before redistribution:

- [FFmpeg legal guidance](https://www.ffmpeg.org/legal.html)
- [FFmpeg nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers)
- [NVIDIA Video Codec SDK](https://developer.nvidia.com/video-codec-sdk)
- [OpenH264](https://github.com/cisco/openh264)
- [Kvazaar](https://github.com/ultravideo/kvazaar)
- [libaom](https://aomedia.googlesource.com/aom/)
- [NASM](https://www.nasm.us/)

This lock and script contain no model weights, runtime DLLs, user media, cloud credentials or release artifacts. The FFmpeg SDK remains a local validation dependency until the full runtime redistribution and license review are complete.

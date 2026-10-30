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

Review the version-specific terms before redistribution:

- [FFmpeg legal guidance](https://www.ffmpeg.org/legal.html)
- [FFmpeg nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers)
- [NVIDIA Video Codec SDK](https://developer.nvidia.com/video-codec-sdk)
- [OpenH264](https://github.com/cisco/openh264)
- [Kvazaar](https://github.com/ultravideo/kvazaar)
- [libaom](https://aomedia.googlesource.com/aom/)
- [NASM](https://www.nasm.us/)

This lock and script contain no model weights, runtime DLLs, user media, cloud credentials or release artifacts. The FFmpeg SDK remains a local validation dependency until the full runtime redistribution and license review are complete.

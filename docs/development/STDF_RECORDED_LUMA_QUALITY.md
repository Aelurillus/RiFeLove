# STDF recorded-luma quality study

This document records the local STDF-R3 quality study completed on **October
31, 2026**. It is a controlled compression experiment, not a product-quality
acceptance and not a claim that STDF improves arbitrary video.

The source is the first 15 frames of the local recorded clip
`work/temporal-restoration/realbasicvsr/data/demo_001.mp4`, decoded and scaled
to 480×360 with FFmpeg's `scale=480:360:flags=lanczos,format=yuv420p,extractplanes=y`
filter. Decoded Y values are clamped to [16,235] and assembled into a grayscale
proxy with constant U/V 128; the controlled re-encodes are tagged BT.709. This
does not validate the original source's color interpretation. The study uses a
continuous 15-frame window with no supplied cuts and a controlled 25 FPS cadence.
The resulting reference is
a decoded grayscale proxy; it is not camera truth, an original uncompressed
master, or a trustworthy ground-truth restoration target.

The recorded local study is
`work/verification/stdf-real-luma-continuous-20261031/quality.json` with source
SHA-256 `d45fef592cb4d9397eee17357599a0cb06f1acc46fc35d363ad45c5958edc097`,
STDF graph SHA-256
`dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`, and tile
policy `stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1`. The CPU study used
ONNX Runtime `CPUExecutionProvider`; each case compares every visible Y pixel.

## Results

The reported delta is output PSNR-Y minus the decoded compressed-input PSNR-Y.
Positive values indicate improvement against this proxy reference. Strength is
the explicit blend strength; `0%` is the unchanged input.

| Source case | Strength | Input PSNR-Y | Output PSNR-Y | Delta | SSIM-Y | Frames improved | Frames worsened |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| H.264, 80 kbps | 0% | 26.5950 | 26.5950 | +0.0000 dB | 0.85170 | 0 | 0 |
| H.264, 80 kbps | 25% | 26.5950 | 26.8390 | +0.2440 dB | 0.85876 | 15 | 0 |
| H.264, 80 kbps | 50% | 26.5950 | 27.0338 | +0.4388 dB | 0.86455 | 15 | 0 |
| H.264, 80 kbps | 100% | 26.5950 | 27.2253 | +0.6303 dB | 0.86935 | 15 | 0 |
| HEVC, QP37 | 0% | 28.1985 | 28.1985 | +0.0000 dB | 0.88269 | 0 | 0 |
| HEVC, QP37 | 25% | 28.1985 | 28.3837 | +0.1852 dB | 0.88787 | 15 | 0 |
| HEVC, QP37 | 50% | 28.1985 | 28.5114 | +0.3130 dB | 0.89202 | 15 | 0 |
| HEVC, QP37 | 100% | 28.1985 | 28.5467 | +0.3483 dB | 0.89434 | 14 | 1 |

The H.264 case improved all 15 frames at all nonzero strengths. The HEVC case
improved all 15 frames at 25% and 50%, while 100% improved 14 frames and made
one frame worse. The study also includes a clean proxy case: every nonzero
strength changed and worsened all 15 frames against the clean reference. That
case is the reason the control remains explicit and defaults Off.

## Native center check

For each case, the native checker processed the center frame (index 7) with a
complete visible-pixel output and independently compared it to the CPU tiled
oracle. All three native checks passed the recorded protocol. The H.264 case
had maximum Y8 difference 1, mean absolute error
`5.787037037037037e-06`, and 1 changed sample across 172,800 pixels. The HEVC
case had maximum Y8 difference 0 and 0 changed samples across 172,800 pixels.
The clean proxy had maximum Y8 difference 1. These checks recorded 20 CUDA
convolution kernels and 9 CUDA GridSample kernels, with no CPU image fallback;
they are native adapter/oracle checks, not a CUDA FP32 tensor-error study.

The native report for the H.264 case is
`work/verification/stdf-real-luma-continuous-20261031/h264-80kbps-native/report.json`.
The HEVC report is
`work/verification/stdf-real-luma-continuous-20261031/hevc-qp37-native/report.json`.
The full CPU-readable matrix remains in `quality.json`; no sparse sample was
used for the CPU metrics.

## Offline reproduction

The following command performs the CPU proxy study into a new evidence directory.
It does not download or upload media and does not launch the native checker:

```powershell
& work/model-tools/Scripts/python.exe tools/validation/stdf_real_luma_quality.py `
  --video work/temporal-restoration/realbasicvsr/data/demo_001.mp4 `
  --graph models/cache/stdf-r3-y7-sdr8-local-eval/model.onnx `
  --ffmpeg work/codec-ffmpeg-build-x86asm/ffmpeg-install/bin/ffmpeg.exe `
  --output work/verification/stdf-real-luma-new-evidence `
  --width 480 --height 360 --frames 15 --threads 2
```

Always choose a fresh output directory and retain failures. The recorded native
checks additionally supplied `--native-checker` and `--runtime`; their exact
resolved commands, checker digests and profiles are preserved in `quality.json`.
Those checks require a separately scheduled GPU run and are not implied by the
CPU-only command.

## What this proves

- The tested STDF graph can produce complete CPU tiled output for this
  480×360, 15-frame, Y-only controlled study.
- The controlled H.264 and HEVC re-encodes show a measurable proxy improvement
  at selected strengths, with the largest measured average gain at H.264 100%.
- Native CUDA center-frame output matches the independent tiled CPU oracle under
  the recorded byte-output protocol, with critical CUDA kernels observed.
- The current r17 implementation has an explicit, default-off compression
  strength and an explicit, default-off bounded tiling path.

## What this does not prove

- It does not prove camera-quality restoration, real ground-truth recovery,
  natural-color or chroma quality, HDR behavior, audio/video timing, or scene
  cut detection.
- It does not prove that 100% strength is safe: the HEVC case contains one
  worsened frame, and the clean proxy worsens at every nonzero strength.
- It does not prove a complete 15-frame native GPU quality run; the native
  protocol checks one entire center frame per case while the CPU metrics cover
  every visible pixel of all 15 frames. It also does not prove full-frame UHD
  network equivalence.
- It does not prove TensorRT support, automatic compression selection, schema
  12 persistence, long-video reliability, or broad NVIDIA compatibility.
- It does not support a Topaz-equivalence claim or public redistribution of the
  STDF weights. The descriptor continues to label the weights as local
  evaluation material with unverified redistribution rights.

Use this record to tune future experiments and to keep the UI wording honest;
it is not a default-quality promise.

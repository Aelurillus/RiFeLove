# STDF-R3: local CPU export and compression controls

Date: 2026-10-31. Status: **CPU feasibility demonstrated; not a native feature,
shipping model, automatic restoration preset, or product-quality acceptance**.
The subsequent [isolated native CUDA feasibility check](STDF_R3_CUDA_FEASIBILITY.md)
also passes small-tensor parity/provider/error checks; it does not add a native
app adapter or change the quality/rights limitations here.
No OSS, uploads, user videos, GPU execution, app UI, queue or packaging changes
are involved in this experiment.

## Source, task and rights

The [author's STDF repository](https://github.com/ryanxingql/stdf-pytorch) is
pinned to `8bedc89e840117f2634cf5183a7eb9441c302360`. Its
[network](https://github.com/ryanxingql/stdf-pytorch/blob/8bedc89e840117f2634cf5183a7eb9441c302360/net_stdf.py)
and [R3 configuration](https://github.com/ryanxingql/stdf-pytorch/blob/8bedc89e840117f2634cf5183a7eb9441c302360/option_R3_mfqev2_1G.yml)
use seven ordered luminance frames, radius 3, deformable temporal fusion and a
center-frame residual. This is compressed-video enhancement, not RIFE frame
interpolation, Gaussian denoising or an arbitrary detail-generation model.
Training uses MFQEv2 HEVC HM16.5 low-delay P at QP37. Other encoders/quality
levels are separate domains and must be tested independently.

The upstream [license](https://github.com/ryanxingql/stdf-pytorch/blob/8bedc89e840117f2634cf5183a7eb9441c302360/LICENSE)
is Apache-2.0; the README separately points to BasicSR/DCNv2 notices. The network
is imported from the local clean checkout, not copied into RifeLove. The new
portable operator is independently implemented using public PyTorch primitives;
it does not compile, embed or copy the upstream CUDA extension.

The author's [v1.0.0 release](https://github.com/ryanxingql/stdf-pytorch/releases/tag/v1.0.0)
links `exp.zip` (8,251,701 bytes), from which only
`exp/MFQEv2_R3_enlarge300x/ckp_290000.pt` was extracted for local evaluation.

- Release archive SHA-256: `566a7cce34a587c14fd0b413ba816f2a42f1ea63d6edda3ac2c525d8d25feb88`.
- Selected checkpoint SHA-256: `d28e0b30082ecbbaeb7b8436968079875c97d882101cc4a79114d33eca2e1ec7`.
- Derived ONNX SHA-256: `dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`.

An explicit independent checkpoint-redistribution grant and training-data rights
have **not** been established. The checkpoint and derived graph stay in ignored
local evaluation storage. Do not put them in installers, portable packages,
Git, model mirrors or any upload. The code license is not used as a substitute
for weight-rights evidence.

## Implemented feasibility path

`tools/model_export/stdf_r3_portable.py` verifies the source revision, clean
working tree and selected checkpoint SHA-256; deserializes tensor weights with
`weights_only=True`; and imports the actual upstream topology. It temporarily
substitutes only its DCNv2 wrapper during import and leaves upstream files
unchanged.

The new operator implements STDF's stride-1, dilation-1, groups-1, odd-kernel
same-padding contract as nine bilinear `GridSample` operations plus a 1x1
convolution. Deformable groups and interleaved dy/dx offsets are preserved. It
supports no broader DCNv2 API. The original source kernel's interleaved layout
was reviewed, but the original CUDA extension was not executed.

The 1,564,328-byte graph contains standard ONNX operators at opset 17, with nine
`GridSample` nodes and no custom-op domain. Input is FP32 `[1,7,H,W]` Y code
values divided by 255, with H/W divisible by eight; output is FP32 `[1,1,H,W]`
center Y and preserves the author's unclamped residual. The fixture evaluator
pads spatial edges when necessary and clamps before 8-bit output. This is a
luminance interface; there is no registered native adapter or qualified
RGB/range/chroma/HDR conversion in this work.

Nine independent CPU operator comparisons cover zero/fractional/large offsets,
seven deformable groups, multiple channels per group, batch 2 and one-pixel
height. Maximum error against `torchvision.ops.deform_conv2d` was `2.09e-7`
(predefined limit `1e-5`). Full pretrained-network comparisons at 64x64, 96x64
and 192x128 used separate torchvision-operator, portable-PyTorch and ONNX Runtime
CPU paths. Maximum ONNX-versus-torchvision-network error was `1.14e-6`
(predefined limit `2e-5`). Exporting again produced the same graph SHA-256.
These are numerical checks, not a comparison against the original CUDA build.

Developer environment: torch 2.8.0+cpu, torchvision 0.23.0+cpu, ONNX 1.17.0,
ONNX Runtime 1.22.0. Optional dependencies are in
`tools/model_export/requirements.stdf-cpu.txt`; Python is not an app dependency.
Pytest passed **7/7** tests for operator parity, explicit scene-boundary windows,
invalid geometry and the SSIM identity/perturbation control.

## Measured compression and clean controls

`tools/validation/stdf_r3_cpu_quality.py` generates nine 192x128 neutral-chroma
SDR frames at 30 FPS, including gradients, fine texture, thin lines, a moving
object and a known hard cut at frame 4. It encodes using software OpenH264,
Kvazaar and libaom, verifies decoded frame counts, and runs only
`CPUExecutionProvider`. Seven-frame windows clamp at the known cut and clip
edges; repeating the center frame gives a temporal-context ablation. This does
not test a scene detector or native timeline integration.

| Generated input | Input Y PSNR | Restored Y PSNR | Input Y SSIM | Restored Y SSIM |
| --- | ---: | ---: | ---: | ---: |
| Clean reference | infinite | 44.03 dB | 1.00000 | 0.99637 |
| OpenH264 16 kbps | 30.91 dB | 31.31 dB | 0.89910 | 0.90630 |
| OpenH264 80 kbps | 37.48 dB | 37.26 dB | 0.98074 | 0.98229 |
| Kvazaar HEVC QP37 | 33.81 dB | 34.21 dB | 0.95770 | 0.96632 |
| Kvazaar HEVC QP27 | 42.97 dB | 41.37 dB | 0.99332 | 0.99408 |
| libaom AV1 CRF40 | 43.04 dB | 40.86 dB | 0.99513 | 0.99386 |

The SSIM definition is valid-center 11x11 Gaussian sigma 1.5, population
covariance, data range 255. It is not the FFmpeg 8x8 implementation. The report
also records edge-gradient errors, motion-subtracted temporal-error differences,
fixed restoration blends 0.25/0.5/1 and repeated-center ablations. The latter
changed output by 0.75–1.14 Y levels RMS, confirming real temporal-context use;
this is not a perceptual temporal-stability score.

Severe H.264 and HEVC examples improved by about 0.4 dB, while clean, lighter
compression and AV1 controls show regression. A fixed half-strength blend
improved this one 80 kbps H.264 example to 37.70 dB, but still degraded other
controls; it is not an automatic-strength solution. Full-strength clean input
changed by 1.60 Y levels RMS and up to 46 levels at one sample. Higher SSIM on
some cases does not negate worse PSNR or edge behavior. **Default enabling is
not justified**.

Inference-only CPU throughput ranged about 6.1–7.0 FPS for these tiny frames
during concurrent CPU work. It excludes encoding, decode, setup and RIFE;
it cannot be extrapolated to 1080p, GPU performance or end-to-end app speed.
Kvazaar is not the training HM encoder. Grayscale synthetic results cannot
establish natural footage, text/HUD integrity, color, blind quality, detail
recovery, long-video stability, VFR or Topaz-equivalent behavior.

## Local evidence and reproduction

Source and selected weights are under ignored
`work/temporal-restoration/stdf-r3` and `stdf-r3-weights`. Exact acquisition
asset/member hashes are in `stdf-r3-weights/acquisition.json`.

```powershell
& work/model-tools/Scripts/python.exe -m pytest tests/model_validation/test_stdf_r3_portable.py -q
& work/model-tools/Scripts/python.exe tools/model_export/stdf_r3_portable.py `
  --source work/temporal-restoration/stdf-r3 `
  --checkpoint work/temporal-restoration/stdf-r3-weights/ckp_290000.pt `
  --output work/verification/stdf-r3-new-export
& work/model-tools/Scripts/python.exe tools/validation/stdf_r3_cpu_quality.py `
  --graph work/verification/stdf-r3-new-export/stdf-r3-y7-fp32.onnx `
  --ffmpeg work/codec-ffmpeg-build-x86asm/ffmpeg-install/bin/ffmpeg.exe `
  --output work/verification/stdf-r3-new-quality
```

Both commands refuse to replace existing evidence directories. The current
reference is `work/verification/stdf-r3-portable-cpu-20261031-v3/feasibility.json`;
complete fixture results, exact software encoder commands, FFmpeg hash/version
and generated data identities are in
`work/verification/stdf-r3-quality-cpu-20261031-v2/quality.json`. These local
reports, models and generated clips are intentionally not committed.

## Remaining gates

Before native integration: independently validate the original STDF CUDA kernel
or establish a stronger reference, extend the isolated CUDA proof to realistic
media/memory sizes (TensorRT remains unqualified), integrate the separately
[validated seven-Y-frame adapter](STDF_R3_LUMA_ADAPTER.md) with actual decoder
extents/scene bounds, and qualify cancellation/checkpoint replay. No
production core changes are made here. Then evaluate authorized real content,
the actual HEVC training-domain reference, representative resolutions, artifacts
and temporal consistency. A content-adaptive compression/detail estimator and
independent relative controls remain separate unfinished work; never derive a
user-facing confidence claim from the reference-only metrics above. Weight
rights must be resolved before any redistribution.

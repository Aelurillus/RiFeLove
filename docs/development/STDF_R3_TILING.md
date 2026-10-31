# STDF-R3 high-resolution tiling qualification

Date: 2026-10-31. Status: **CPU consistency qualification only**.

This document records the first high-resolution study for the reviewed STDF-R3
ONNX graph. It answers whether overlapped tiles can reproduce the graph's
whole-frame result closely enough to justify a future native 1080p/4K path. It
does not add tiling to the product pipeline, claim arbitrary-video quality, or
qualify a CUDA memory budget.

The graph is the local developer artifact
`work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx` with
SHA-256
`dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`.
The checkpoint and weights remain local evaluation material.

## Why a fixed small halo is unsafe

The reviewed network has a three-level U-Net STDF feature extractor, nine
deformable `GridSample` operations, and an eight-convolution quality-enhancement
network. The offset head is itself predicted from the U-Net output. A manual
receptive-field calculation gives the offset head an approximate 75-pixel
static receptive-field width, or a 37-pixel radius, before learned deformable
sampling is considered. The eight quality convolutions add another eight
pixels of radius.

The deformable offsets are not bounded by the ONNX graph. The local offset probe
measured the following full-frame generated-window values:

| Shape | Case | Max radial offset | P99.9 radial offset |
| --- | --- | ---: | ---: |
| 192×128 | large displacement edge | 27.43 px | 21.10 px |
| 192×128 | moving textured square | 22.56 px | 20.54 px |
| 384×256 | large displacement edge | 36.79 px | 27.55 px |
| 384×256 | moving textured square | 36.03 px | 24.21 px |

These measurements are evidence for test selection, not a proof of a safe
halo. Combining the measured 36.8-pixel offset with the static dependencies
would give a conservative dependency radius near 82 pixels, but that estimate
still does not prove behavior for unseen inputs. A tile also recomputes its
offset field, so the whole-frame offset map cannot simply be copied into tiles.

## Tile boundary policy

The comparison tool clamps a tile's source extent to the real frame. It never
materializes synthetic pixels outside the frame, because ordinary convolutions
on those pixels would create bias/ReLU activations and differ from the graph's
own boundary padding. The model receives each clamped source tile, and only the
requested core is copied back.

Tile widths, heights, and halos must be multiples of eight. This is required by
the exported U-Net concat geometry. Invalid dimensions fail before inference.
The core is written exactly once, and seam metrics are reported separately from
non-seam pixels.

## CPU whole-frame versus tile results

The oracle is the same ONNX graph run on the complete input. The cases cover
independent noise, smooth texture, moving textured and hard-edge objects, a
high-frequency checkerboard, and a hard scene cut. Strict consistency in these
tables means maximum FP32 absolute error ≤3e-5 and maximum rounded 8-bit
difference ≤1.

At 384×256, six cases were compared for each tile policy:

| Core tile | Halo | Worst max FP32 error | Worst seam MAE | Worst 8-bit difference | All cases strict |
| --- | ---: | ---: | ---: | ---: | --- |
| 128×128 | 0 | 0.14430 | 0.02259 | 36 | No |
| 128×128 | 32 | 0.00551 | 0.000305 | 1 | No |
| 128×128 | 40 | 0.000409 | 0.00000959 | 1 | No |
| 128×128 | 48 | 0.0000147 | 0.000000591 | 1 | Yes |
| 128×128 | 64 | 0.0000145 | 0.000000551 | 1 | Yes |
| 192×128 | 0 | 0.14430 | 0.02199 | 36 | No |
| 192×128 | 32 | 0.00400 | 0.000312 | 1 | No |
| 192×128 | 40 | 0.000409 | 0.0000110 | 1 | No |
| 192×128 | 48 | 0.0000150 | 0.000000724 | 1 | Yes |
| 192×128 | 64 | 0.0000146 | 0.000000679 | 1 | Yes |

The 192×128 run gives the same trend. At 128×192, halos 48 and 64 also
passed all six generated cases; halo 32 still failed the strict gate on noise,
fine texture, motion, and cut cases. The high-frequency checkerboard and hard
scene cut are deliberate stress cases rather than a claim about visual quality.

The evidence supports **halo 48 as the smallest tested candidate for this exact
graph and these generated inputs**, with halo 64 as the more conservative
experimental default. It does not authorize a product guarantee for arbitrary
video. A future implementation must rerun the oracle comparison after any
graph, runtime, precision, provider, padding, or tile-size change.

## High-resolution and memory implications

The existing native CUDA feasibility run measured a 384×256 whole inference at
about 1,666 MiB of device-wide use including the desktop. That number is not a
per-process allocation trace, and no 1080p or 4K full-frame run is claimed here.
The model's intermediate feature maps and deformable sampling dominate memory;
the seven-frame FP32 input alone is about 55 MiB at 1920×1080 and 222 MiB at
3840×2160. On a 6 GiB GTX 1660 SUPER, full-frame 1080p/4K inference must be
treated as an unqualified OOM risk until measured in the selected native
provider. A bounded tile path is the plausible route, with an explicit memory
budget and no silent reduction of tile quality.

This research did not start a CUDA tiling run because another agent is using the
GPU for the production backend. The next GPU qualification must measure the
same tile sizes and halos with the actual native provider, include session and
arena overhead, and verify that the peak remains within the selected budget.

## Memory-bounded larger-frame experiment

`tools/validation/stdf_tiling_bounded.py` generates deterministic tiles from
global coordinates and retains only the current seven-frame source tile and
output tile, plus one copied core for comparing halo policies. It can run a
small whole-frame oracle shape, or sample
selected tiles from a larger shape without allocating a full-frame input or
output. Its host-tensor number excludes ONNX Runtime's provider arena and is
therefore a lower bound for a native GPU budget.

The 768×512 oracle run used a 384×256 core and all four tiles. For moving
texture and large displacement, halo 48 stayed below `1.1e-5` maximum FP32
error. The high-frequency checkerboard reached `3.95e-5` maximum error at halo
48 and `4.36e-5` at halo 64, while remaining within one rounded 8-bit level.
This is a useful warning: increasing halo does not guarantee a monotonic
floating-point maximum for every shape and content, so the strict oracle gate
must remain empirical.

The sampled 1080p and 4K runs used a 384×256 core, six deterministic tiles per
case, and compared halo 48 with halo 64:

| Shape | Case | Halo 48 vs 64 max error | Seam max error | Host tensor peak / tile | Halo 48 time / tile |
| --- | --- | ---: | ---: | ---: | ---: |
| 1920×1080 | moving texture | 5.96e-7 | 4.92e-7 | 5.16 MiB | 0.748 s |
| 1920×1080 | large displacement | 1.12e-7 | 4.47e-8 | 5.16 MiB | 0.735 s |
| 1920×1080 | fine checkerboard | 2.48e-5 | 1.60e-5 | 5.16 MiB | 0.756 s |
| 3840×2160 | moving texture | 1.34e-7 | 5.96e-8 | 5.16 MiB | 0.852 s |
| 3840×2160 | large displacement | 2.01e-7 | 5.96e-8 | 5.16 MiB | 0.832 s |
| 3840×2160 | fine checkerboard | 2.33e-5 | 1.48e-5 | 5.16 MiB | 0.850 s |

These larger-frame numbers compare two tile policies, because a whole-frame
oracle is deliberately absent. They show that the sampled tile outputs are
stable between halo 48 and 64, including the seam strips, but they do not prove
that either policy matches a hypothetical full-frame 1080p/4K inference. The
CPU timings also do not predict CUDA throughput. A full 1080p frame contains 25
tiles and a full 4K frame contains 90 tiles at this core size; all-tile native
GPU timing and provider memory remain outstanding.

The runner now compares halo policies one tile at a time. Its earlier prototype
kept NumPy views for every tile, which retained the corresponding complete
output allocations and made memory grow with image size. The new
`peak_tracked_host_tensor_bytes` includes the live source and output tensors
and the copied comparison core; generator and metric temporaries, any whole-frame oracle,
and provider allocations remain outside that estimate. A 192×128 checkerboard
oracle run covered all six 64×64 tiles; halo 48 reached a maximum FP32 error of
`5.22e-6` and no changed rounded 8-bit pixels. This is a small regression check,
not a 1080p/4K or GPU qualification.

## Product boundary recommended by this evidence

1. Keep whole-frame execution when the selected GPU budget accepts it.
2. If a bounded tile path is needed, require dimensions divisible by eight,
   clamp outer extents, use a graph-bound tile policy, and retain the full
   oracle-derived halo in the job identity.
3. Do not silently switch to halo 0–40. The CPU study shows visible numerical
   seam risk even when average error is small.
4. If the memory budget cannot fit a qualified tile, fail the task with an
   actionable OOM/unsupported-size error. Do not claim 1080p/4K support from
   the 384×256 feasibility run alone.
5. An adaptive halo based only on observed offset magnitudes is not qualified.
   Offset prediction depends on the tile, and the static U-Net context remains
   part of the dependency. Any future adaptive strategy needs a separate
   whole-frame oracle study with fail-closed limits.

## Reproduction

The standalone tools write only to a fresh evidence directory and refuse to
overwrite one. They do not download or package model files.

```powershell
& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_compare.py `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-tiling-cpu-20261031-new `
  --height 256 --width 384 --tile 128 128 --tile 192 128 `
  --halo 0 --halo 32 --halo 40 --halo 48 --halo 64

& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_offset_probe.py `
  --source work/temporal-restoration/stdf-r3 `
  --checkpoint work/temporal-restoration/stdf-r3-weights/ckp_290000.pt `
  --output work/verification/stdf-tiling-offset-20261031-new `
  --height 256 --width 384

& work/model-tools/Scripts/python.exe -m pytest `
  tests/model_validation/test_stdf_tiling.py -q

& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_bounded.py `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-bounded-cpu-20261031-new `
  --shape 512 768 --reference-shape 512 768 `
  --shape 1080 1920 --shape 2160 3840 `
  --tile 384 256 --halo 48 64 --sample-tiles 6
```

The completed local evidence directories are
`work/verification/stdf-tiling-cpu-20261031-03`,
`work/verification/stdf-tiling-cpu-20261031-04`,
`work/verification/stdf-tiling-offset-20261031-01`, and
`work/verification/stdf-tiling-offset-20261031-02`.

The larger-frame bounded evidence is in
`work/verification/stdf-bounded-cpu-20261031-02`,
`work/verification/stdf-bounded-cpu-20261031-03`,
`work/verification/stdf-bounded-cpu-20261031-1080p`, and
`work/verification/stdf-bounded-cpu-20261031-4k`.

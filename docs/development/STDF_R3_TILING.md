# STDF-R3 high-resolution tiling qualification

Date: 2026-10-31. Status: **opt-in native CUDA tiled evaluation; generated-input parity recorded**.

This document retains the original CPU research below and records the later
native implementation and complete-output CUDA checks at the end. The desktop
now has a separate, default-off tiled evaluation switch. Generated-input parity
does not establish arbitrary-video recovery quality, whole-frame UHD network
equivalence or an exact per-process CUDA allocation bound.

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
not a 1080p/4K or GPU qualification. A repeated 4K checkerboard run sampled six
of 90 tiles: the two halo policies differed by at most `2.34e-5` FP32 and one
8-bit level. The tracked tensor peaks were 5.53 MiB for halo 48 and 6.38 MiB
for halo 64, with the exclusions above. There was no full-frame oracle; this
is still CPU-only consistency evidence.

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
`work/verification/stdf-bounded-cpu-20261031-4k`. The streaming regression
checks are `work/verification/stdf-bounded-streaming-r14-20261031` and
`work/verification/stdf-streaming-4k-check-r14-20261031`.

## Independent fixed-policy fixture and geometry checks (2026-10-31)

`tools/validation/stdf_tiling_reference.py` is a separate CPU developer oracle
for the proposed/native fixed **256x128 core, halo64** policy. It does not call
the product CUDA backend, modify application settings, download a graph, or
qualify a release. The earlier research scripts and tables above keep their
original tile sizes and eight-aligned input restrictions.

The new oracle independently partitions the **visible** frame, clamps source
extents to real pixels, and repeats the final visible row/column only for the
single global right/bottom ceil8 padding. It never inserts a synthetic halo
beyond the top/left boundary or an internal tile edge. Non-eight-aligned source
frames are explicit tests, not silently resized inputs. Model input dimensions
remain at most 384x256 for halo64. Each axis is limited to 3840 and visible area
to 8,294,400 pixels, so both 3840x2160 and 2160x3840 fit; a 3840-square frame does
not.

The new 31-test CPU suite covers complete geometry and native pure-policy
agreement, not just sampled coordinates. It constructs a per-pixel coverage map
and checks every core for the complete landscape UHD grid (**255 tiles**) and
portrait UHD grid (**270 tiles**), 1080p, 854x480, 1918x1078, an odd 519x263 frame with seven-pixel
tail cores, and a 1x1 source. A small standalone C++20 test driver compiles only
`src/restoration/stdf_tile_policy.hpp` and compares all emitted native tiles
against the independent Python plan. It does not build or change the application,
CMake, inference backend, UI, or packaging. That cross-language test is skipped
when the optional local g++ compiler/header is unavailable; in the recorded
local run it ran and passed.

### Raw fixture/checker contract

`fixture` writes a fresh directory containing `fixture.json`, seven real frames
with three raw strided `yuv420p` plane files each, and compact visible center
Y/U/V files. Y, U and V strides deliberately exceed visible width by 5, 3 and 7
bytes, respectively; padding bytes are 250. Odd chroma dimensions use ceil
division. The integer-only patterns cover global-coordinate texture, moving
texture, hard edges and a moving fine checkerboard, including distinct real
frame boundaries. Per-plane byte counts and SHA-256 hashes are recorded.

Frame indices are 100 through 106; center slot is 3. PTS is the exact rational
`(index*3003 + (index%3)*77)/90000`, with duration equal to the next exact PTS
difference and alternating `timestampRecovered` flags. No float timestamps or
generated CFR replacement is used. A round trip must retain these exact center
values, color tags and every visible U/V byte.

The separate strided-plane interchange is protocol
`rifelove-stdf-tiled-fixture-v1`: the native side reads the fixture manifest and
writes compact `output-y.u8`, `output-u.u8`, `output-v.u8` plus `output.json`
containing `width`, `height`, `center_index`, integer-rational `center_pts` and
`center_duration`, `timestamp_recovered`, and the unchanged `color` object.
`compare` checks **every** visible sample and exact metadata; its regression test
deliberately corrupts the last pixel to ensure a sparse check cannot pass. An
optional `--expected-y` supplies real CPU expected restored bytes instead of the
center identity output. This interchange is prepared and unit tested; **an
end-to-end native strided-plane round trip has not yet been recorded in this
section**. Native runtime inference, cancellation and buffer-lifetime evidence
must be added separately after checker integration.

The actual CUDA checker now uses the luma bridge described in
`tools/validation/STDF_TILED_RAW_PROTOCOL.md`, rather than `fixture.json`:
`native-fixture` writes `manifest.json`, all seven compact visible planes in
`input-y7.u8`, and a complete `reference-y.u8`. The CPU session is constructed
from SHA-256-verified graph bytes and permits only `CPUExecutionProvider`.
The manifest is published only after every reference pixel is written, and
records the graph path/hash, generator hash, input/reference hashes, strength
and the explicit oracle kind. Whole-frame references are bounded to small
shapes; large references independently execute **every** halo64 tile. Large
tiled references establish native/policy consistency, not equivalence to a
hypothetical whole-frame UHD neural result. Partial/failed generation retains
its raw files and `cpu-reference-report.json`, without a consumable manifest.

`native-compare` independently reads **every** `native-y.u8` byte, verifies
fixture hashes/counts and cross-checks the native report's identity, all-tile
count and error summaries. The byte gate is maximum difference <=1, allowing
CPU/CUDA quantization boundaries. This luma bridge does not externally round
trip chroma, PTS or duration; those remain internal native checker assertions.
The strided-plane protocol above is retained as a separate stronger data
contract, rather than claiming its metadata assertions ran through this bridge.
Unit tests exercise successful generation, a last-pixel corruption, a dishonest
native summary and preserved failure evidence with a fake center session;
those unit tests are not real model or GPU evidence.

### New real CPU inference evidence

The graph hash is the same reviewed `dcd09f...aad0721` identity above. Small
whole-frame CPU reference and the independent halo64 tiled result were compared
at every one of **77,809 visible pixels per case**, including non-eight-aligned
right/bottom padding:

| Shape | Case | Cores | Maximum FP32 error | Maximum rounded byte difference |
| --- | --- | ---: | ---: | ---: |
| 391x199 | moving texture | 4 | 9.239e-6 | 1 |
| 391x199 | fine checkerboard | 4 | 1.079e-5 | 1 |
| 391x199 | hard edge | 4 | 9.239e-6 | 1 |
| 391x199 | coordinate texture | 4 | 7.451e-6 | 1 |

All four passed the explicit CPU consistency gate of maximum FP32 error <=3e-5
and maximum rounded byte difference <=1. Raw whole-frame/tiled FP32 arrays and
the expected quantized Y remain in the evidence directory. This is numerical
consistency on generated inputs, not proof of visual recovery quality.

A separate large-coordinate study compares **halo64 with halo96**. Halo96 is
only a CPU research comparator: it can use a 448x320 input and is outside the
384x256 production tile envelope. These are **sampled tile-policy comparisons,
not complete 4K frame inference and not a full-frame oracle**:

| Shape | Case | Evaluated / all tiles | Compared core pixels | Max FP32 error | Max byte difference |
| --- | --- | ---: | ---: | ---: | ---: |
| 3840x2160 | fine checkerboard | 6 / 255 | 192,512 | 1.617e-5 | 1 |
| 2160x3840 | moving texture | 6 / 270 | 104,448 | 3.040e-6 | 0 |

Reports explicitly contain `all_tiles_evaluated: false`,
`full_frame_oracle: false`, `gpu_used: false`, selected tile indices, total tile
count and the number of compared pixels. Passing these samples does not establish
complete landscape/portrait UHD numerical correctness, CUDA memory use, throughput
or arbitrary-content halo safety. Real all-tile native execution and GPU memory/
stability qualification remain outstanding.

Reproduction (fresh output paths required):

```powershell
& work/model-tools/Scripts/python.exe -m pytest `
  tests/model_validation/test_stdf_tiling_reference.py -q

& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py fixture `
  --output work/verification/stdf-raw-fixture-new --width 519 --height 263 --case moving_texture

& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py oracle `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-small-oracle-new --width 391 --height 199 `
  --case checkerboard --threads 2

& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py large `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-sampled-uhd-new --width 3840 --height 2160 `
  --case checkerboard --sample-tiles 6 --threads 2

# Complete native checker luma fixture, with a small whole-frame CPU oracle:
& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py native-fixture `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-native-small-new --width 391 --height 199 `
  --case checkerboard --oracle whole --threads 2

# Every UHD visible output pixel is generated by the independent CPU tile policy:
& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py native-fixture `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-native-uhd-new --width 3840 --height 2160 `
  --case checkerboard --oracle tiled --threads 2

# After the separate serial native CUDA run has produced its evidence directory:
& work/model-tools/Scripts/python.exe tools/validation/stdf_tiling_reference.py native-compare `
  --fixture work/verification/stdf-native-uhd-new --actual $NativeEvidenceDirectory
```

Evidence is under `work/verification/stdf-tiled-reference-python-20261031/`.
The raw odd/tail fixture is `fixture-519x263-motion/`; the four `oracle-*` folders
contain complete small-frame comparisons and the `sampled-uhd-*` folders contain
the explicitly partial large-coordinate study. Pytest XML/logs include the native
pure-policy geometry check. The runners retain numerical failure arrays and error
reports in their fresh evidence directories instead of deleting failed results.

## Native CUDA all-tile evaluation

The later native implementation uses the fixed identity
`stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1`. It is explicitly selected;
the original bounded whole-frame path and its runtime signature remain separate.
Every graph input stays at or below 384x256. Visible sources must have each axis
at most 3840 and area at most 8,294,400 pixels. The adapter reuses one seven-plane
FP32 tile input and one FP32 prediction, then commits the completed visible Y8
frame. The original center U/V owners, exact PTS, duration and recovery metadata
are retained. Cancellation, incomplete/nonfinite output or a failed later tile
discards the entire unfinished frame. These failure contracts use injected
inference tests; they are not a claim of prompt preemption of a running GPU kernel.

Serial native CUDA runs on the local GTX 1660 SUPER, ORT 1.22.0, FP32 graph above
completed the following **entire output frames**, followed by an independent
Python read of every native output byte and fixture hash:

| Source | Generated case | Cores | CPU oracle | Maximum Y8 difference | Seam maximum | Native restore seconds |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| 391x199 | checkerboard | 4 | complete whole-frame | 1 | 1 | 0.406 |
| 519x263 | moving texture | 9 | complete whole-frame | 1 | 0 | 0.389 |
| 854x480 | hard edge | 16 | complete independent tiled | 0 | 0 | 0.808 |
| 1918x1078 | coordinate texture | 72 | complete independent tiled | 1 | 1 | 3.574 |
| 1920x1080 | moving texture | 72 | complete independent tiled | 1 | 0 | 3.579 |
| 3840x2160 | checkerboard | 255 | complete independent tiled | 1 | 0 | 14.261 |
| 2160x3840 | moving texture | 270 | complete independent tiled | 1 | 0 | 14.956 |

All cases passed the maximum one-level Y8 difference gate. The native checker
also asserts padded source strides, nonuniform rational timestamps, unchanged
center chroma pointer/stride/extents and lifetime after the request is destroyed.
Its provider gate recorded the required 20 CUDA Conv and 9 CUDA GridSample
kernels and no CPU image fallback. Those metadata/lifetime checks are internal
native assertions; the luma interchange independently verifies pixels only.
No CUDA FP32 tensor-error measurement is claimed by this byte-output checker.

The landscape UHD adapter reported 3,145,728 bytes of reusable FP32 buffers,
13,004,456 bytes of owned graph/tensor/Y8 payload and 87,204,516 bytes of retained
decoded plane spans. These values exclude ORT arena/activations and allocator
overhead. Separate process/device sampling observed 660,828,160 bytes peak host
working set and 1,911,554,048 bytes total device use for that run. The device
number includes the desktop and any other processes; sampling can miss a peak
and is not an exact per-process VRAM bound. Native restore timings exclude
session construction, media decode, RIFE, encode and final validation.

Evidence is local at `work/verification/stdf-tiled-native-20261031/`: `matrix.json`,
per-case `report.json`, native raw outputs, ORT profiles, independent readback
reports and sampled process/device counters. Raw media and profiles are not
source-control artifacts. The complete Release suite passed 211/211 tests;
the model-tool CI command passed 44 tests locally. The new Python contracts are
included in the existing CPU-only GitHub workflow, without models or GPU downloads.

These generated-input results qualify an opt-in evaluation implementation.
They do not establish an unbounded halo guarantee, whole-frame UHD network
equivalence, restoration superiority, representative real-footage quality,
long-video reliability, HDR support or a general GPU compatibility matrix.

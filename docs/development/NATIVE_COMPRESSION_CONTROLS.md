# Native local compression-restoration evaluation

The quality card has a dedicated compression-restoration model selector and a
separate Off / 25% / 50% / 100% strength selector. It does not reuse FastDVDnet
denoise or the super-resolution control. A verified local STDF pack can be
registered and selected in the model center; its model default is saved in the
local preferences, while strength starts off and resets off on model refresh.
The task captures its own model directory, descriptor SHA-256 and strength into
the persistent queue. The manual path began with schema 10; the committed
schema 11 additionally pins the explicit tiled flag and tile-policy identity.

This is a **local evaluation path**, not a general restoration-quality claim.
Its whole-frame gate requires NVIDIA CUDA and explicitly tagged, limited-range
BT.709 SDR8 `yuv420p` input no larger than **384 × 256**. HDR, unknown metadata,
other pixel formats and larger inputs remain blocked for that path.

A separate **高清分块（实验；较慢）** opt-in is present in the recorded r17
evaluation package. It uses fixed 256×128 cores, 64-pixel halos and global
right/bottom edge padding to multiples of 8; graph inputs remain at most
384×256. Each visible axis may be up to 3840 and total visible area up to
8,294,400 pixels. The policy is
`stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1`. Complete generated UHD
frames have independent CPU-oracle and serial CUDA byte-output evidence, as
recorded in [STDF tiling](STDF_R3_TILING.md). This does not establish whole-frame
UHD equivalence, arbitrary halo sizes, HDR, long-video quality or a general GPU
compatibility matrix.

Model, strength and tiling remain explicit choices; strength defaults Off and
tiling defaults off. Batch import validates each input independently. Supported
composition follows the existing backend chain: STDF, optional source-rate
FastDVDnet, RIFE, optional output-rate super-resolution. Schema 12 adds an
experimental automatic-compression mode; the r19 isolated local QA exercised
it on a short generated block-pattern video, not general real-media quality.
Frames without positive confidence bypass the model and never update smoothing
state, even when the configured minimum-confidence threshold is zero.

## Verification

`tests/native/native_compression_test.hpp` adds source-contract and native
workflow tests to the existing control and processing test targets. A separate
actual-EXE driver accepts:

```powershell
build/windows-release/rifelove-native-exe-test.exe <fresh-portable-package>/RifeLoveNative.exe <tagged-small-sdr8-fixture.mp4> <new-output.mkv> --compression
```

This drives the real model/strength HWNDs, selects 50%, starts the real queue and
checks the completed record through read-only SQLite, including its pinned STDF
identity, independent processing mode, software decode, RIFE calls and validated
frame counts. It requires a fresh portable QA data directory and never operates
on an existing user queue. These checks establish functional behavior; they do
not claim visual-quality or rendered screenshot acceptance. A separate
[recorded-luma study](STDF_RECORDED_LUMA_QUALITY.md) measures every CPU output
pixel of a continuous 15-frame grayscale proxy and checks one native center per
case. Its decoded proxy is not camera ground truth and does not qualify a
natural-color video restoration workflow.

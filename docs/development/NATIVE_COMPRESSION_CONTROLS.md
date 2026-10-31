# Native local compression-restoration evaluation

The quality card has a dedicated compression-restoration model selector and a
separate Off / 25% / 50% / 100% strength selector. It does not reuse FastDVDnet
denoise or the super-resolution control. A verified local STDF pack can be
registered and selected in the model center; its model default is saved in the
local preferences, while strength starts off and resets off on model refresh.
The task captures its own model directory, descriptor SHA-256 and strength into
the existing schema-10 persistent queue.

This is a **small-frame local evaluation path**, not finished 720p/1080p/4K
compression restoration. Its gate requires NVIDIA CUDA and explicitly tagged,
limited-range BT.709 SDR8 `yuv420p` input no larger than **384 × 256**. HDR, unknown
metadata, other pixel formats and larger inputs remain blocked. Batch import
validates each input independently. Supported composition follows the existing
backend chain: STDF, optional source-rate FastDVDnet, RIFE, optional output-rate
super-resolution. No high-resolution tiling qualification is implied.

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
not claim visual-quality or rendered screenshot acceptance.

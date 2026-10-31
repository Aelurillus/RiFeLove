# STDF tiled native fixture protocol v1

`rifelove-validate-stdf-tiled-native graph.onnx runtime-directory fixture-directory fresh-evidence-directory`

This checker exercises the real opt-in native CUDA tiled backend. It does not
download or package weights. The evidence directory must not exist. The parent
runner owns GPU serialization and qualification; merely building the checker
does not run inference.

The fixture directory contains:

- `manifest.json` with the fields below.
- `input-y7.u8`: exactly `7 * width * height` unsigned 8-bit luma bytes, seven
  visible planes in chronological order, row major, no row padding.
- `reference-y.u8`: exactly `width * height` final restored luma bytes, produced
  independently by CPU ORT from the same pinned graph and strength. Clip model
  prediction to `[0,1]`, blend with original center, clip to `[0,255]`, then round
  nonnegative values with `floor(x + 0.5)` (C++ `lround`, not ties-to-even).

```json
{
  "schemaVersion": 1,
  "width": 854,
  "height": 480,
  "case": "moving-texture",
  "graphSha256": "dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721",
  "tilePolicy": "stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1",
  "strength": 1.0,
  "oracle": "cpu-independent-tiled-global-pad8"
}
```

Allowed oracle kinds are `cpu-whole-frame-global-pad8` for a complete whole-frame
reference and `cpu-independent-tiled-global-pad8` for an independently implemented
CPU tiler using core256x128 + halo64. The latter checks native parity with that
tile policy; it does **not** prove high-resolution whole-frame equivalence.

Pad the full original image's right/bottom edge once to ceil8, matching the old
full-frame adapter. Tile cores partition only visible output. For a core beginning
at `(x,y)`, each visible size is `min(coreSize, sourceSize - origin)`. Input begins
at `(max(0,x-64), max(0,y-64))`, ends at
`(min(ceil8(width),x+visibleCoreWidth+64), min(ceil8(height),y+visibleCoreHeight+64))`.
No additional tile-edge padding is permitted. Use seven temporal source planes
with this exact same spatial extent. Input shape is `[1,7,tileHeight,tileWidth]`,
and only the requested visible core is copied to the output.

The checker creates owned padded-stride Y/U/V frames from the raw luma, retains
center U/V unchanged, uses exact nonuniform rational timestamps, and verifies
metadata/owner identity after inference. It writes `native-y.u8`, `report.json`
and the actual ORT provider profile. A successful comparison requires maximum
8-bit native/reference difference <=1. Mean absolute error, changed pixels and
seam-region difference are reported separately. This is native/CPU consistency,
not general perceptual-quality qualification. CUDA versus CPU FP32 tensor error
is not measured by this Y8 result protocol.

The report includes policy ID, actual tile count, graph-input maximum dimensions,
adapter float/payload and retained plane-span proofs. Those payload values
exclude ORT/device activations/arenas, decoder allocation padding not covered by
visible spans, standard-library allocator overhead, and other process/device
allocations. Measure actual native process/device peaks independently.

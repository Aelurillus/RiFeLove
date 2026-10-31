# STDF-R3: independent seven-Y-frame adapter

Status: local development validation, 2026-10-31. This is a reusable preparation
for a future `IVideoRestorationBackend`, not an enabled application feature or a
redistributable model pack. No `src/`, root CMake, queue, UI, shared test or
packaging changes were needed. GPU was not used in this phase.

The [CPU reference](STDF_R3_CPU_FEASIBILITY.md) establishes the real pretrained
network/portable operator identity and limited synthetic quality evidence.
The [native CUDA probe](STDF_R3_CUDA_FEASIBILITY.md) independently establishes
small-shape CUDA execution and its cancellation limitations. Neither document
establishes generic compression recovery, perceptual temporal acceptance or
permission to redistribute the checkpoint/derived graph.

## Reusable native interface

`tools/validation/stdf_luma_adapter.hpp` uses the actual existing
`restoration::Request`, `restoration::Result` and `media::DecodedFrame` types.
It does not duplicate fake app types or link a production backend.

| Interface | Responsibility |
| --- | --- |
| `VerifiedWeights::load(path, maximumBytes)` | Read a bounded local graph, SHA-256 with Windows BCrypt, reject any identity other than the independently qualified graph, retain immutable bytes. |
| `plan(request, boundedPlanes, sceneBounds, weights, limits)` | Validate source-rate deblock, seven explicit temporal slots, real PTS, scene bounds, planar color/strides/extents and payload limits; retain a private snapshot of validated frames and owners. |
| `pack(plan)` | Produce FP32 `[1,7,Hpad,Wpad]` encoded Y/255, explicit scene/clip-edge repetition and right/bottom edge repetition to the next multiple of eight. |
| `complete(plan, prediction, strength, elapsed)` | Reject cancellation/shape/nonfinite errors; crop, clamp, fixed-strength encoded-Y blend and round to bytes; return new owned Y and retained center U/V. |
| `run(weights, request, boundedPlanes, sceneBounds, strength, inference, limits)` | Validate/pack/dispatch/check stop/complete with no error or OOM fallback; strength zero bypasses inference and returns the original center storage. |

The inference callback receives `(verifiedGraphBytes, inputSpan, inputShape,
outputSpan, stopToken)`. A future ORT adapter should create a session from the
verified bytes, not reopen an unverified model pathname. CUDA stream/provider,
RunOptions and synchronization still belong in that runtime adapter. The
callback must fill the complete output span or throw; the span starts as NaN,
so unwritten values fail instead of becoming silent black output. Stop checks
immediately before dispatch, after callback return and before
publication discard a prediction completed after cancellation, without claiming
that queued CPU/GPU kernels were immediately preempted.

`BoundedPlanes` explicitly carries accessible decoder-plane byte spans. A raw
pointer, stride and `shared_ptr<const void>` cannot prove the accessible buffer
length. The spans must come from trusted decoder storage; forging spans cannot
be made safe by this adapter. Positive strides are required. Private immutable
`Plan` geometry prevents callers from forging tensor counts or changing a
request's pointers after validation. Its frame snapshot retains the original
owners independently of the caller's request lifetime.

## Exact input/output policy

- Only owned software-decoded planar `yuv420p`, 8-bit, tagged BT.709
  primaries/transfer/matrix, limited range (`tv`), without HDR/mastering/content
  light data, and explicit `bt709-sdr8` profile. Unknown tags are errors; NV12,
  full range, BT.470BG, 10-bit and HDR are not silently converted.
- Seven slots with three before/after center. Only real frames carry timestamps;
  missing slots are explicit counts. Actual source indices must be adjacent
  and exact rational PTS strictly increasing; VFR spacing is allowed. Known
  duration must be positive. No PTS/duration is synthesized or normalized.
- All real frames must be inside supplied inclusive scene bounds. Missing
  history is permitted only at the actual scene start, and missing future
  context only at a declared scene/clip end. One-frame scenes repeat their
  single real Y plane in all slots. The adapter does not infer a scene boundary.
- Spatial padding repeats the last visible row/column, never stride padding.
  Chroma dimensions use ceil division for odd source sizes. No RGB conversion,
  range expansion or linear-light interpretation is introduced.
- For strength `s`, cropped finite network code `p` and original byte `y`, output
  is `lround(clamp(y + s * (255 * clamp(p,0,1) - y),0,255))` with float32 math.
  Legal-range studio black/white are not stretched. As in the research network,
  output is clipped to byte storage, not artificially restricted to 16..235.
  A fixed blend is a policy control, not a learned confidence/compression score.
- Output copies the center frame's exact index, dimensions, PTS, duration,
  recovery flag and color metadata. U/V pointers, strides and visible bytes are
  unchanged; replacement storage retains the original center storage until the
  restored frame is released. New Y has a compact positive stride.

The immutable graph SHA-256 is
`dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`.
Its independently verified checkpoint/source identity is in the linked CPU
report. Weights and derived graph remain in ignored local evaluation storage.

## Explicit limits

Qualified source maximum is **384x256**, with padding never exceeding that
shape. Callers may lower configured dimensions but cannot widen this ceiling.
Default adapter-owned payload budget is **16 MiB** and separate retained
decoded-plane-span budget is **16 MiB**. The model file read cap defaults to
**2 MiB**. Checked arithmetic and positive-stride span geometry reject overflow
before tensor allocation. Budget equality is accepted; exceeding by one byte is
rejected. No CPU replacement, automatic tiling or zero-filled output is returned
on a limit/inference error.

Owned payload accounting covers verified graph bytes, seven float input Y
planes, one float output plane and compact replacement center Y. Retained span
accounting sums the visible-through-last-row extents of each real Y/U/V plane.
These are explicit **per-invocation payload** limits, not a total RAM/VRAM cap:
ORT session/activation/arena allocations, C++ metadata/allocator overhead,
hidden decoder allocations, repeated invocations/concurrency and caller-owned
references are excluded. Production scheduling needs additional enforced
runtime and process/device budgets.

## Reproducible standalone CPU validation

The native harness builds independently using real `src/common/rational.cpp`
read-only. It does not enter the app's build graph. With the existing MinGW,
nlohmann-json headers and local CPU tooling:

```powershell
New-Item -ItemType Directory -Force work/verification/stdf-adapter-local-build | Out-Null
g++ -std=c++20 -O2 -Wall -Wextra -Werror -static -Isrc `
  -IC:/itzming/dependences/nlohmann-json-3.12.0/include `
  tools/validation/stdf_luma_adapter_check.cpp src/common/rational.cpp -lbcrypt `
  -o work/verification/stdf-adapter-local-build/stdf_luma_adapter_check.exe
& work/model-tools/Scripts/python.exe tools/validation/stdf_luma_adapter_cpu.py `
  --probe work/verification/stdf-adapter-local-build/stdf_luma_adapter_check.exe `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --output work/verification/stdf-adapter-new-cpu-check
& work/model-tools/Scripts/python.exe -m pytest `
  tests/model_validation/test_stdf_r3_portable.py `
  tests/model_validation/test_stdf_cuda_profile.py `
  tests/model_validation/test_stdf_luma_adapter.py -q
```

Choose a fresh output directory; existing evidence is never removed/replaced.
The driver verifies the graph before constructing an explicit **CPU-only** ORT
1.22.0 session from bytes. An independent NumPy oracle reconstructs native
packed tensors from separately emitted visible source Y planes. The actual
network runs on each tensor, then its prediction is read by the native harness
for `complete()`. NumPy independently verifies final cropped/quantized Y and
byte-for-byte center U/V; native checks validate metadata/lifetime/cancellation.

The latest complete local evidence is
`work/verification/stdf-adapter-cpu-20261031-v6/adapter-cpu.json`, with native
`prepare.json` and `complete.json` siblings. Reports/tensors/binaries are ignored
local data, not committed release assets.

| Fixture | Visible source | Padded Y tensor HxW | Result |
| --- | --- | --- | --- |
| Interior with unequal rational PTS gaps | 67x49 | 56x72 | Exact packing, crop/quantization, center metadata and U/V. |
| Declared hard-cut scene start | 65x51 | 56x72 | Three explicit missing-history slots repeat the first scene frame. |
| Declared hard-cut scene end | 73x57 | 64x80 | Three explicit missing-future slots repeat the last scene frame. |
| One-frame scene | 9x7 | 8x16 | All seven slots repeat the single real frame; center timing is preserved. |
| Qualified spatial ceiling with odd edges | 383x255 | 256x384 | Exact visible-plane padding/crop and unchanged chroma at maximum padded size. |

All five actual CPU model round-trips pass; each real prediction is also
explicitly rejected after its stop token is requested. The standalone native
harness passes **54** named checks covering malformed colors/windows/planes,
cut boundaries, weights, budgets, overflow, finite results, blending,
cancellation before/after a fake successful inference, propagation of native
runtime errors and center-owner lifetime. The separate offline pytest suite is
**18/18 passing** (portable operator, CUDA-profile classifier and seven new
oracle checks). These tests establish adapter correctness on the described
synthetic data, not restored quality on real consumer videos.

## Remaining integration and acceptance work

1. Resolve weight rights before packaging, uploads or a redistributable model
   descriptor. The source-code license alone does not resolve checkpoint terms.
2. Implement an explicitly experimental, default-off native temporal backend
   with verified session bytes, truthful provider diagnostics, explicit runtime
   memory budget and stop/discard semantics; no generic denoise-control reuse.
3. Wire trusted decoded-plane extents and actual scene-bounded temporal-window
   scheduling. Bind source/checkpoint/graph, color/padding/strength, scene
   policy, provider/runtime and replay configuration to job identity.
4. Compare interrupted/uninterrupted output at safe recovery boundaries; replay
   sufficient real prefix context and validate exact frame count/PTS/duration.
5. Independently qualify normal-resolution memory/latency, real compressed
   content, temporal/perceptual quality and any tile/halo policy. Deformable
   sampling makes an arbitrary guessed halo inadequate. TensorRT, native
   decoder/encoder flow and larger GPU shapes remain unqualified.
6. Keep domain-specific quality claims honest: the existing small synthetic
   study improved severe H.264/HEVC controls but damaged other controls. It does
   not justify a universal default, detail-recovery promise or Topaz equivalence.

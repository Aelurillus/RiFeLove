# STDF-R3: native ONNX Runtime CUDA feasibility

Date: 2026-10-31. Status: **native GPU numerical/provider/error-boundary checks
passed on one GPU; no production adapter or packaged feature**. The GPU was
released after the checks. No production source, existing shared tests, app UI,
queue, release package or model manifest was changed by this work. Model and
checkpoint redistribution remains unresolved; local evidence stays ignored.

This follows [the CPU/source/quality experiment](STDF_R3_CPU_FEASIBILITY.md).
Its negative clean/light-compression quality controls remain applicable; GPU
execution does not qualify a general compression/detail-restoration preset.

## What actually ran

`tools/validation/stdf_gpu_probe.cpp` is a standalone C++20 developer probe.
It loads the existing explicit runtime directory using Windows DLL APIs,
requires ONNX Runtime **1.22.0**, and uses its native C API/C++ header wrapper.
No Python GPU package or app backend abstraction is involved.

The selected graph SHA-256 remains
`dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`.
The driver writes graph, executable, ORT, CUDA-provider, CUDA-runtime and cuDNN
DLL hashes, along with GPU name/driver, tensor identities and native profiles.
Reference outputs are recomputed from the pinned actual upstream network with
independent torchvision CPU deformable convolution at every tested shape.

Hardware: NVIDIA GeForce GTX 1660 SUPER, 6 GiB. Provider registration explicitly
selects CUDA; TensorRT is not registered. Options use FP32, `use_tf32=0`,
`cudnn_conv_algo_search=DEFAULT`, `cudnn_conv_use_max_workspace=0`, a 1 GiB arena
budget and `kSameAsRequested` arena growth. TensorRT was unnecessary for this
feasibility gate and remains unqualified.

## Pixel parity, time and memory

The predefined limits were max FP32 absolute error `<3e-5` and max rounded
8-bit difference `<=1`. Four runs per shape include the first inference;
run times include CPU tensor upload and output return, but exclude session
construction, media decoding/encoding and RIFE.

| Shape W×H | Max FP32 error | Max 8-bit difference | First run | Last warm run | Observed device memory high |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64×64 | 3.58e-7 | 0 | 135 ms | 7.30 ms | 1,356 MiB |
| 192×128 | 1.08e-6 | 0 | 142 ms | 18.14 ms | 1,430 MiB |
| 384×256 | 3.55e-6 | 1 | 156 ms | 54.73 ms | 1,666 MiB |

Session construction took 92–95 ms after DLL/CUDA setup. Device-memory numbers
are `nvidia-smi` samples across the whole GPU, including existing desktop/app
allocations; they are **not** exact per-process allocation peaks. Independent
`cudaMemGetInfo` snapshots are also recorded. At 384×256 the CUDA-reported
device-wide use grew from about 1,010 MiB before session construction to about
1,350 MiB after runs. Sampling can miss brief peaks. No 1080p, 4K, long-video,
tile-memory or end-to-end performance claim follows from these small tensors.

## Actual provider assignments

Native ORT profiling recorded all twenty Conv nodes, three ConvTranspose nodes,
nine GridSample nodes and image activation/math operations executing through
`CUDAExecutionProvider` in each completed inference. Across four runs, it
recorded 1,152 CUDA kernel events and 96 CPU events per tested shape.

The CPU work is explicit: 24 events per run for scalar/small-shape `Gather`,
`Unsqueeze`, `Mul`, `Concat`, `Cast` and `Div`. Profile output types/shapes were
checked; outputs are int64 scalars/vectors of at most six elements or FP32
scalars. No restoration convolution/GridSample or float image-sized operation
ran on the CPU. Provider tags describe native ORT kernel execution; profile
durations are not an independent CUDA-event/Nsight hardware timing trace.

Setting `session.disable_cpu_ep_fallback=1` **refuses session construction**
because these control nodes are assigned to CPU. This is recorded rather than
hidden: the validated path is CUDA image compute plus explicit CPU shape work,
not an all-nodes-on-GPU claim. The source graph was not silently replaced with
a CPU-only session or another inference model.

Offline profile-classification tests reject CPU Conv/GridSample execution and
reject float image math disguised as shape work. The combined CPU/operator and
profile suite passes **11/11** tests.

## Cancellation and error boundaries

- A pre-terminated native RunOptions request is rejected with the terminate
  flag error.
- A request issued 2 ms into an already-running 384×256 inference may **not
  preempt already-enqueued CUDA work**. The first experiment correctly failed
  a stricter preemption expectation. The follow-up probe explicitly observes
  the request before Run returns, discards that inference result and checks no
  cancelled result is written. It completed the in-flight call in about 55 ms.
  This is bounded waiting in a tiny case, not an immediate GPU-abort guarantee.
- Clearing RunOptions termination and making a separate valid call succeeds;
  its written output matches the independent CPU reference within the original
  tolerance. A cancellation must still cause the future to fail/drop its result
  in a production backend, even if ORT returns successfully.
- Device ID 99 fails explicitly; there is one available CUDA device. No output
  tensor is published and no CPU-only replacement is created.
- An invalid 64×65 shape fails at the actual U-Net Concat geometry. A future
  adapter must validate/pad dimensions before dispatch, rather than present
  this raw runtime error to users.
- A 4 MiB CUDA arena budget fails at Conv allocation with a specific requested
  allocation/budget error. No output is published and no silent CPU inference
  occurs. This arena limit is not a complete process/VRAM limit.

The evidence completion status is
`passed_native_cuda_boundary_checks` under the explicit discard-on-cancel
policy. It does not certify the app task manager, container publication,
checkpointing or resume, none of which are invoked by this isolated probe.

## Reproduction and evidence

Build without modifying the project CMake graph:

```powershell
g++ -std=c++20 -O2 -static `
  -I C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0/include `
  -I C:/itzming/dependences/nlohmann-json-3.12.0/include `
  tools/validation/stdf_gpu_probe.cpp `
  -o work/verification/stdf-gpu-probe-build-20261031/stdf_gpu_probe.exe
& work/model-tools/Scripts/python.exe tools/validation/stdf_cuda_reference.py `
  --probe work/verification/stdf-gpu-probe-build-20261031/stdf_gpu_probe.exe `
  --runtime out/RifeLoveNative-local-20261031-r12/runtime `
  --graph work/verification/stdf-r3-portable-cpu-20261031-v3/stdf-r3-y7-fp32.onnx `
  --source work/temporal-restoration/stdf-r3 `
  --checkpoint work/temporal-restoration/stdf-r3-weights/ckp_290000.pt `
  --output work/verification/stdf-cuda-new-check
& work/model-tools/Scripts/python.exe -m pytest `
  tests/model_validation/test_stdf_r3_portable.py `
  tests/model_validation/test_stdf_cuda_profile.py -q
```

The driver refuses an existing output directory. Local complete evidence is
`work/verification/stdf-cuda-reference-20261031-v2/cuda-reference.json`; native
per-case reports/profiles/stderr and input/output tensors are its siblings.
The original stricter preemption counterexample is preserved in
`work/verification/stdf-cuda-reference-20261031/cuda-reference.json`.
No tests in this document need to be interpreted as permission to package or
upload the model, generated tensors, videos or binaries.

## Proposed native integration boundary; not implemented

1. Add a specifically named experimental **source-rate deblock** adapter,
   separate from FastDVDnet Gaussian denoising and output-rate SR. Reuse the
   `IVideoRestorationBackend` real decoded-frame contract with three real
   neighbors on each side, explicit missing-context counts and preserved
   center PTS/duration/metadata. Do not reuse the RIFE adjacent-frame interface.
2. Initially require software-decoded planar YUV420P 8-bit, tagged BT.709 SDR
   with limited range. Consume the encoded Y plane as code values divided by
   255; retain the center frame's U/V unchanged. The returned frame needs owned
   replacement Y storage plus retained original center storage for U/V. Avoid
   an unnecessary RGB round-trip. Full range, BT.470BG, NV12/NVDEC, 10-bit and
   HDR need their own conversion/quality cases before support is declared.
3. Reuse scene-bounded real-frame scheduling and explicit edge repetition.
   Never invent neighbor timestamps or cross a cut. Pad spatial edges to a
   multiple of eight, crop back, and preserve exact center timing/side data.
   Test actual VFR/gaps and end-of-file handling, not just index windows.
4. Bind source weight/graph, color convention, padding/window/cut/strength
   settings, actual provider, runtime versions and any tile policy to job and
   checkpoint identity. Existing generic model-pack schema and temporal window
   code are useful starting points, but their adapter allowlists/pipeline path
   must be reviewed before adding support. Replay the required real prefix on
   recovery and compare interrupted/uninterrupted decoded pixels and timestamps.
5. Allocate a bounded seven-Y-frame input, use the existing selected CUDA
   device/stream, and prevent unqualified TensorRT fallback or CPU image
   compute. Current proof uses whole-frame CPU-backed tensors up to 384×256;
   tile/halo/full-frame parity and realistic-size memory/cancellation tests must
   precede normal-resolution use. Model-deformable offsets make an arbitrary
   guessed halo inadequate.
6. Check stop tokens before dispatch, immediately after native Run/sync and
   before result publication. Discard a result completed after cancellation;
   never claim RunOptions alone preempts queued CUDA kernels. Preserve explicit
   OOM/device/invalid-request errors and publish no success output on failure.
7. Keep default off with an experimental/task-domain explanation. Add explicit
   fixed-strength blending only after its encoded-Y/range semantics are tested.
   An independent compression estimator, relative user adjustment, detail
   reconstruction and perceptual/temporal acceptance remain unfinished work.
   Resolve weight rights before any redistributable pack or release.

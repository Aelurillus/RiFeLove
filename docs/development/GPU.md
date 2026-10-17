# Native CUDA and ONNX Runtime development

The application runtime remains native C++20. Python/PyTorch are used only for source review, export and reference generation.

## Driver primitives and evidence

`src/gpu/cuda_device.*` loads `nvcuda.dll` from Windows System32, retains a primary context, creates a nonblocking stream and owns allocations through reference-counted buffers. Allocation requests are bounded by a tracked budget. The default buffer budget is the smaller of one quarter of currently free memory and 1 GiB. This does not bound other libraries' allocations or all VRAM usage.

Local hardware: GTX 1660 SUPER, compute 7.5, driver 560.94, driver CUDA API maximum 12.6, 6 GiB VRAM. Four hardware tests passed in Debug and Release: actual device/memory query, budget release/reallocation, host/device and device/device transfer equality with range/cancel errors, and buffer lifetime after the device wrapper closes. A separate test rejects negative device indices before driver loading. Driver API support is not an installed Toolkit version or a codec capability claim.

## Native backend boundary

`IInterpolationBackend` exposes asynchronous futures over owned normalized RGB FP32 NCHW device frames. `OnnxCudaBackend` serializes work on a worker thread with a bounded queue, uses the owned CUDA stream and ONNX I/O binding, and accepts exclusive reusable outputs. Endpoints use exact device copies. Request cancellation uses stop tokens and ONNX termination; error/cancel paths synchronize the stream before releasing storage. Replacement and profiling require an idle backend. Device-buffer aliases must remain immutable until their future completes.

ONNX Runtime uses its public C function table; no MSVC C++ object crosses into the MinGW application. Model bytes are size-bounded and SHA-256 verified before `CreateSessionFromArray`; external-data packs are not supported by this adapter. Paths must stay within the pack, including resolved links. Descriptor hashes are integrity checks, not signatures or proof of an author's identity. Trusted catalogs/custom-model policy and sandboxing still belong to the future model manager.

The native backend is compiled and its file/hash tests pass. Actual GPU model execution has not yet passed validation: the initial native probe correctly failed because `cublasLt64_12.dll` was missing. No CPU session was substituted. Runtime acquisition is in progress. Do not interpret the compiled target or CPU reference result as GPU numerical parity.

## Pinned local setup

`tools/development/onnx-cuda-sdk.lock.json` pins ONNX Runtime GPU 1.22.0, its C header, nlohmann JSON 3.12.0, CUDA runtime 12.8.90, cuBLAS 12.8.4.1, cuFFT 11.3.3.83 and cuDNN 9.8.0.87. The provider's PE imports were inspected to identify the required DLLs; cuFFT is a binary dependency even though RIFE itself does not request FFT operations. NVIDIA wheels supply native DLLs only; the application does not import Python. The setup helper uses cached assets by default and requires `-AllowDownload` for acquisition. It does not change system PATH, the driver or installed system runtimes.

```powershell
.\tools\development\setup-onnx-cuda.ps1 -AllowDownload -DependencyRoot C:/itzming/dependences
cmake --preset windows-dev -DRIFELOVE_BUILD_ONNX_CUDA=ON -DRIFELOVE_ONNX_ROOT=C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0 -DRIFELOVE_JSON_ROOT=C:/itzming/dependences/nlohmann-json-3.12.0
cmake --build --preset windows-dev
.\build\windows-dev\rifelove-validate-cuda.exe models/cache/rife-4.25-fp32-sdr8-v2 C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0/lib build/windows-dev/native-cuda-validation.json
```

After the separate numerical qualification and complete native run succeed, set `RIFELOVE_TEST_RIFE_PACK` to the local pack directory to include `OnnxCuda.NativeReference` in CTest. The validator verifies reference hashes, compares multiple shapes/timesteps, tests exact endpoints, cancellation, invalid inputs and output reuse, and parses ONNX profiling. Convolution, transpose convolution and GridSample must actually execute on CUDA. CPU shape operations are reported separately. TF32 is disabled. The ORT arena has its own 1 GiB default limit, which is not a total-process VRAM cap.

The driver currently exposes CUDA 12.6 while this ORT package uses CUDA 12.8 libraries; this combination still requires a real compatibility test. NVENC separately fails because the current FFmpeg SDK requires a newer NVENC API. No driver update or successful codec fallback is claimed. TensorRT, FP16, CUDA Graph, buffer pooling, NVDEC, tiling and end-to-end video interpolation remain open.

ONNX Runtime and nlohmann JSON use MIT with their retained notices. NVIDIA DLLs use proprietary SDK licenses, not the application's MIT license. The bundled license grants local installation/use subject to its terms; final distribution must separately audit the permitted components and requirements. No vendor DLLs or model weights are committed.

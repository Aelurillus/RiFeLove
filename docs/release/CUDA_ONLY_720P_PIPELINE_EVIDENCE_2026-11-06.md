# CUDA-only 720p pipeline evidence — 2026-11-06

## Measurement

This is a local, synthetic throughput smoke on one Windows workstation. The input was a 2-second 1280x720, 30 fps FFV1 BT.709 clip. Each run generated 120 frames at 60 fps with the packaged RIFE 4.25 FP32 SDR8 model and H.264 NVENC. The pipeline decoded and validated all 120 output frames and the 2-second boundary.

GPU: NVIDIA GeForce GTX 1660 SUPER, compute capability 7.5, 6 GiB VRAM. Model graph SHA-256: 8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3. Input SHA-256: dfbb8fb8dbf1a4da9338410eca33b56936541da37b7796ab7906e4d43f91c795.

| Runtime profile | Backend | Engine cache | End-to-end elapsed | End-to-end output rate |
| --- | --- | --- | ---: | ---: |
| CUDA-only package runtime | ONNX Runtime CUDA FP32 | Not applicable | 12.595 s | 9.53 fps |
| CUDA-only package runtime, repeat | ONNX Runtime CUDA FP32 | Not applicable | 12.626 s | 9.50 fps |
| Full TensorRT runtime | TensorRT FP32 | Cold build | 57.483 s | 2.09 fps |
| Full TensorRT runtime | TensorRT FP32 | Warm cache | 8.758 s | 13.70 fps |

The warm TensorRT run was about 44% faster end to end than the mean of the two CUDA-only runs. Its inference wait was 1.716 seconds, compared with a 7.959-second CUDA-only mean. TensorRT cold time includes engine construction; tactics requiring more than the card's available 6 GiB were skipped. The cache was isolated under the ignored local benchmark output directory.

The CUDA-only runs emitted 56 ONNX Runtime cuDNN Conv fallback warnings and a warning that 28 graph memcpy nodes were added. This points to a worthwhile CUDA-path performance investigation, but does not by itself justify changing the current algorithm or its numerical policy. Color conversion/restoration was about 1.72 seconds in each run, and scene analysis about 0.52 seconds.

## Boundaries

The benchmark used an internal development CLI with RIFELOVE_BUILD_TESTS=OFF and RIFELOVE_BUILD_NATIVE_DESKTOP=OFF. That CLI is not included in the native package. Both runs used software FFV1 source decoding because this synthetic input has no CUDA decoder; NVDEC performance is not measured. The clip is synthetic and no PSNR, SSIM, LPIPS, or human visual-quality rating was measured. This evidence does not qualify real-footage quality, a clean-machine install, the native GUI's complete import-to-output flow, or redistribution licensing.

On this GTX 1660 SUPER, the full TensorRT runtime remains the performance-oriented profile. CUDA-only is a smaller package option with lower measured throughput. Neither profile is accepted as a public release by this smoke alone. Machine-readable reports and encoded outputs were retained locally under the ignored directory out/cuda-only-pipeline-check.
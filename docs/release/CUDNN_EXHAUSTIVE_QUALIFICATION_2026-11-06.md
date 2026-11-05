# cuDNN exhaustive search qualification — 2026-11-06

## Change under evaluation

ONNX Runtime 1.22 CUDA sessions use `cudnn_conv_algo_search=EXHAUSTIVE` instead of `DEFAULT`. `cudnn_conv_use_max_workspace=0`, FP32 inference, and TF32 disabled are unchanged. This qualification does not cover TensorRT sessions.

## Numerical reference

The CUDA validator used the packaged RIFE 4.25 FP32 SDR8 model, SHA-256 `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`, with the existing six-case reference pack. All six cases passed the existing absolute-error and PSNR gates. Worst case: max absolute error `0.00153041`, PSNR `88.5063 dB`. The machine-readable result is `build/cudnn-exhaustive-validation/exhaustive-cuda-reference.json` (local build output, not committed).

## End-to-end throughput

Both configurations used the same synthetic BT.709 FFV1 input (1280×720, 30 fps, 2 seconds), GTX 1660 SUPER (compute capability 7.5, 6 GiB), model pack, CUDA-only package runtime, and H.264 NVENC output. Each result produced and decoded 120 frames at 60 fps, with zero declared end-boundary error.

| CUDA convolution search | Run | Elapsed | Output rate | Inference wait |
| --- | ---: | ---: | ---: | ---: |
| DEFAULT | 1 | 12.595 s | 9.527 fps | 7.941 s |
| DEFAULT | 2 | 12.626 s | 9.504 fps | 7.976 s |
| EXHAUSTIVE | 1 | 8.688 s | 13.812 fps | 4.039 s |
| EXHAUSTIVE | 2 | 8.632 s | 13.902 fps | 4.013 s |

Mean end-to-end throughput increased from `9.516` to `13.857` output fps (`+45.6%`). Mean inference wait decreased from `7.959` to `4.026` seconds.

## Output comparison and limits

The two EXHAUSTIVE outputs were each compared against the first DEFAULT output after encoding. Across 120 frames, FFmpeg reported average PSNR of `53.05`/`53.12 dB` and SSIM `0.998593`/`0.998618` for the two comparisons. This is an encoded-output consistency check, not a ground-truth perceptual-quality score.

This is a single-GPU synthetic throughput qualification. It does not establish performance or memory behavior on other GPU generations, real-footage visual quality, CUDA cold-start behavior across a hardware matrix, or native GUI import-to-export acceptance. The setting should remain subject to wider hardware and real-footage qualification before describing CUDA performance as universally improved. The development CLI used for the pipeline smoke is not part of the shipped package.

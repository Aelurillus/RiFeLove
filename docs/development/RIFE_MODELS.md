# RIFE source acquisition and export validation

RIFE 4.25 and 4.26 are the first two locally selectable model packs. This is not a claim that full-resolution GPU interpolation, quality presets, subjective model ranking or final visual quality acceptance is implemented.

## Provenance

The author's [Practical-RIFE README](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/README.md#trained-model) explicitly extends the project's MIT license to the linked model content. The [MIT license](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/LICENSE) names hzwer as the 2021 copyright holder. `models/manifests/rife-4.25.source.json` pins the author-linked Drive archive, a repository commit for the warp helper, the license file and hashes of individual approved files. The archive hash is a first-acquisition integrity pin; the README did not publish a separate authoritative weight checksum.

`models/manifests/rife-4.26.source.json` records the separately linked 4.26 archive at the same pinned upstream commit, its MIT scope statement, first-acquisition archive hash, approved checkpoint/source entries and license. The exporter and source-acquisition tools accept either manifest. Both model packs are locally present, selectable offline and bound to their source manifest and license by SHA-256. These checks establish local integrity and recorded provenance; they are not author signatures. The upstream README recommends 4.25 for most scenes, so 4.25 remains the default; the additional 4.26 entry is an alternate model, not a claim of better quality.

`acquire_rife.py` verifies the complete archive and each allowed entry before extracting only the network source and tensor checkpoint. Other archived files, including compiled Python bytecode and platform metadata, are not part of the reviewed package. All weights and generated engines remain under ignored directories.

## Local developer environment

The upstream installation guidance uses Python 3.11 or earlier. The managed export environment is Python 3.11.17 with pinned CPU PyTorch/ONNX/ONNX Runtime dependencies; it is isolated under `work/model-tools`. Python is not being added to the application's end-user runtime. The setup helper uses a checksum-pinned uv executable and does not add a system PATH entry or register the managed interpreter in the Windows registry.

```powershell
# Initial dependency acquisition requires explicit opt-in; later runs use cached packages.
.\tools\development\setup-model-tools.ps1 -AllowDownload
python .\tools\model_export\acquire_rife.py
.\work\model-tools\Scripts\python.exe .\tools\model_export\export_rife.py
```

If the model archive/source files are missing from `work/downloads`, `acquire_rife.py --allow-download` acquires the pinned sources. Without that flag it stays offline. An existing model file that differs from the pin is not overwritten.

## Export contract and pending verification

The export tool checksum-checks source bytes and compiles those bytes directly, avoiding archived `.pyc` imports. Checkpoint loading uses `torch.load(weights_only=True)` from the verified bytes. The pinned IFNet explicitly excludes the `teacher` and `caltime` training modules; only these prefixes may be removed. Every inference parameter must still match strictly, and missing or other unknown parameters fail. It compares the upstream warp implementation with a cache-free dynamic export variant before ONNX generation.

The prepared validation covers two spatial profiles and three timestep values, retains timestep as an input, compares ONNX Runtime CPU output to upstream PyTorch at a fixed error limit, and publishes an immutable local pack only after all comparisons pass. Packs contain ONNX, license/source metadata, a validation report and raw FP32 reference cases for subsequent native CUDA testing. The adapter expects batch 1, normalized RGB FP32, NCHW and padded spatial dimensions divisible by 64; actual video padding/cropping and color conversion still need pipeline integration.

The local environment is installed and the exporter has been executed. The original strict maximum-error gate of `2e-4` **fails**; its failed report is retained and no strict pack is published. Disabling graph optimization or MKLDNN did not remove the drift. Intermediate-flow diagnostics show small FP32 differences amplified in the final refinement block; an FP64 shadow run also differs from upstream FP32. This is evidence of numerical sensitivity, not a completed visual-quality acceptance.

A separate, explicitly selected `sdr8` numerical profile requires maximum normalized RGB error <= `0.5/255` and PSNR >=80 dB against upstream FP32. This relates the numerical comparison to half an 8-bit code value; it is neither a guarantee of identical rounded pixels nor a perceptual/HDR/10-bit quality claim. The default remains `strict`. Failed gates do not publish model packs. Native CUDA uses the same pack-specific limits and additionally checks actual operator placement.

The model descriptor's input profiles are separate from that numerical-error threshold. Current local packs declare the tagged BT.709 and BT.470BG-family SDR8 and SDR10 profiles because the adapter converts those inputs to the same normalized FP32 NCHW contract. SDR10 has synthetic CPU conversion coverage and a 96x64 end-to-end RIFE/AV1 pipeline regression that checks preserved pixel format, color tags and timing; real-content 10-bit RIFE quality still needs visual review.

```powershell
.\work\model-tools\Scripts\python.exe tools/model_export/export_rife.py --numerical-profile sdr8 --output-dir models/cache/rife-4.25-fp32-sdr8-v2
```

The six local CPU comparisons (64x64 and 128x192; t=0.2,0.5,0.8) passed this separate SDR8 profile: maximum error `0.0012595653533935547`, minimum PSNR `90.2527304312149` dB; warp adapter error was zero. The ONNX SHA-256 is `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`. Artifacts remain ignored under `models/cache`; they are small-profile numerical fixtures, not video quality or performance acceptance.

## Local pack descriptor v2

The exporter writes a version 2 `model.json` with a tensor/input capability block, a license identifier and file hash, a source-manifest path and hash, and a validation-report path and hash. The validation report lists its model digest, numerical profile, tested backends and per-case pass results. `loadModelPack` checks bounded pack-relative paths and verifies all declared hashes before inference; it also confirms that the source license matches the descriptor and that the validation report matches the ONNX digest, profile and backend list. HDR remains explicitly unqualified.

The Model Center displays these claims and reports whether the sidecar hashes agree. A matching hash proves local file consistency against the descriptor; it is not a digital signature or proof of author identity. Legacy schema v1 packs remain supported for existing users, but their sidecar metadata is labeled as not hash-bound. The current exporter records `pytorch_cpu` and `onnxruntime_cpu` only; native CUDA and TensorRT numerical checks are reported separately below rather than inferred from the CPU export report.

Native ONNX CUDA now passes the same six SDR8 reference cases on GTX 1660 SUPER. With the fixed `DEFAULT` cuDNN convolution baseline and TF32 disabled, maximum error is `0.001534104347229004`, minimum PSNR `83.86017808054655` dB. Debug and Release full local suites pass; five additional consecutive Release native runs pass. Profiling proves CUDA convolution, transpose convolution and GridSample execution; the observed CPU tensors contain at most four scalar/shape elements. See [GPU evidence](GPU.md).

Heuristic convolution selection initially passed, then failed a Release comparison at maximum error `0.00231170654296875`. Its gate was not loosened. The CUDA runtime now fixes the documented implicit-precomputed-GEMM baseline; faster algorithms require separate numerical qualification. TensorRT 10.9 FP32 now passes the same six numerical cases on GTX 1660 SUPER, and profiling confirms a fused TensorRT kernel executes the model. This covers only 64x64 and 128x192 fixtures; full-resolution video, FP16, multi-model presets, visual quality, HDR handling and performance acceptance remain required.

Seven source-integrity/checkpoint-policy scenarios are part of normal CTest and also pass under pytest: allow-listed extraction, corrupt archive rejection, offline cache behavior, protection of modified local assets, altered-code rejection before ML import, documented training-only exclusions and missing-inference-parameter rejection. These checks do not substitute for numerical model validation.

The 4.26 ONNX pack also passes six PyTorch CPU reference cases, six native ONNX Runtime CUDA comparisons and six native TensorRT FP32 comparisons on the local GTX 1660 SUPER. Its CPU maximum absolute error is `0.0011979341506958008`; the TensorRT comparison maximum is `0.001828312873840332` with minimum PSNR `85.94 dB`. TensorRT skipped tactics whose requested workspace exceeded the card's 6 GB; the selected engine still ran native TensorRT operators and passed the numerical gate. These small profiles establish an additional model/backend option, not full-resolution or perceptual quality qualification. RIFE 4.25 remains the only model with the recorded end-to-end 720p output and independent PyTorch video comparison.

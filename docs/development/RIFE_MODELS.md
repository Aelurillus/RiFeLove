# RIFE source acquisition and export validation

RIFE 4.25 is the first candidate being prepared. This is not a claim that native GPU interpolation, the complete model manager or final visual quality acceptance is implemented.

## Provenance

The author's [Practical-RIFE README](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/README.md#trained-model) explicitly extends the project's MIT license to the linked model content. The [MIT license](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/LICENSE) names hzwer as the 2021 copyright holder. `models/manifests/rife-4.25.source.json` pins the author-linked Drive archive, a repository commit for the warp helper, the license file and hashes of individual approved files. The archive hash is a first-acquisition integrity pin; the README did not publish a separate authoritative weight checksum.

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

The export tool checksum-checks source bytes and compiles those bytes directly, avoiding archived `.pyc` imports. Checkpoint loading uses `torch.load(weights_only=True)` from the verified bytes and strict state-dictionary matching. It compares the upstream warp implementation with a cache-free dynamic export variant before ONNX generation.

The prepared validation covers two spatial profiles and three timestep values, retains timestep as an input, compares ONNX Runtime CPU output to upstream PyTorch at a fixed error limit, and publishes an immutable local pack only after all comparisons pass. Packs contain ONNX, license/source metadata, a validation report and raw FP32 reference cases for subsequent native CUDA testing. The adapter expects batch 1, normalized RGB FP32, NCHW and padded spatial dimensions divisible by 64; actual video padding/cropping and color conversion still need pipeline integration.

At this checkpoint the source acquisition tool has been run successfully. CPU package installation and export validation are still pending; no passed ONNX parity result is claimed here. Native ONNX CUDA, TensorRT FP32/FP16, multi-model quality presets, full visual tests, HDR handling and performance acceptance remain required even after CPU reference validation succeeds.

Five source-integrity scenarios are part of the normal local CTest run and can also be discovered by pytest: allow-listed extraction, corrupt archive rejection, offline cache behavior, protection of modified local assets, and rejecting altered code before ML libraries are loaded. These policy tests do not substitute for numerical model validation.

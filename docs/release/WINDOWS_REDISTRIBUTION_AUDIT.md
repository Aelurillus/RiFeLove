# Windows redistribution audit

**Status: the current native package is still a local qualification candidate, not a public release.** This inventory records exact package evidence and upstream terms; it is not a legal opinion or an authorization to distribute.

## Most recent historical package candidate

The most recently recorded package is `RifeLoveNative-local-cuda-only-rife-only-2026-11-06-main-96a51b4`. It was built on 2026-11-06 from a tracked source tree matching `main` at `96a51b4b4eca8010d298c67d2ed01fc5321e965d`. Since that build, `main` has advanced beyond the recorded commit; this package does not match the current source tree and must not be used as the release package. Rebuild and requalify a package from the final release commit before distribution.

- Package kind: `local-native-windows-x64`; runtime profile: `cuda-only`; model bundle: `rife-only`.
- 89 files, 2,492,044,028 bytes; no Qt, CLI, user media, or user data.
- `PACKAGE-MANIFEST.json` SHA-256: `5988b9503ced8999f7a479d4171894efa363abdbc13685e9f6f121de3fd3f40b`.
- `RifeLoveNative.exe` SHA-256: `7ad942d10bf75501274a04a3e020ee39af3c91d74ca8e68be209017de6d9f5dd`.
- One bundled model: RIFE 4.25 FP32 SDR8, graph SHA-256 `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`.

The inference runtime inventory contains these 17 DLLs:

- CUDA: `cublas64_12.dll`, `cublasLt64_12.dll`, `cudart64_12.dll`, `cufft64_11.dll`, `cufftw64_11.dll`, and `nvblas64_12.dll`.
- cuDNN 9: `cudnn64_9.dll`, `cudnn_adv64_9.dll`, `cudnn_cnn64_9.dll`, `cudnn_engines_precompiled64_9.dll`, `cudnn_engines_runtime_compiled64_9.dll`, `cudnn_graph64_9.dll`, `cudnn_heuristic64_9.dll`, and `cudnn_ops64_9.dll`.
- ONNX Runtime: `onnxruntime.dll`, `onnxruntime_providers_shared.dll`, and `onnxruntime_providers_cuda.dll`.

This profile excludes TensorRT and its builder resources. It reduces the redistributable inventory but does not clear the remaining CUDA, cuDNN, codec, Microsoft runtime, or clean-machine gates.

## Current license findings

- **RIFE 4.25 model:** the pinned upstream [Practical-RIFE model list](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/README.md#trained-model) states that linked trained-model contents are under the same MIT license as the project. The package carries that pinned MIT license and a source manifest. This is direct license-scope evidence for the linked weights. The upstream author did not publish a checkpoint checksum, so the locally recorded source hashes establish acquisition integrity, not author authentication. The signed publisher catalog and mirror read-back are still absent.
- **CUDA runtime and math libraries:** NVIDIA's [current CUDA SDK agreement](https://docs.nvidia.com/cuda/eula/index.html) permits distribution only for components listed as distributable, incorporated into an application with material additional functionality, app-scoped access, and downstream terms consistent with the agreement. Its CUDA supplement assigns separate audio/video codec licensing responsibility to the distributor. Match every shipped CUDA DLL to the exact source package, version, hash, and applicable attachment; a CUDA package license file alone is not a file-by-file redistribution record.
- **cuDNN 9:** the current development runtime's eight DLLs were SHA-256 matched byte-for-byte to NVIDIA's official cuDNN 9.8.0.87 CUDA 12 Windows archive. The archive is pinned in `tools/development/cudnn-redistrib-9.8.0.lock.json` (675,349,654 bytes; SHA-256 `d8a23705e3884b137b7e05449fb2b61bfa524e7cfc3fda80743d633f423c6ce4`); its canonical 29,879-byte `cudnn/LICENSE.txt` is checked in at `packaging/windows/licenses/NVIDIA-cuDNN-9.8.0.87-LICENSE.txt` (SHA-256 `7936e1f9f2a6d9deea237aee6aa7f1abd032d34bcc1c35de1e6516b223d09487`). The packager now rejects cuDNN files or license text with different hashes and records their exact provenance in `PACKAGE-MANIFEST.json`. NVIDIA's [cuDNN license](https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/LICENSE.txt) describes redistribution conditions; this hash/source match is provenance evidence, not a legal approval. Review those conditions together with the rest of the CUDA profile before distribution. NVIDIA's [redistribution index](https://developer.download.nvidia.com/compute/cudnn/redist/) lists cuDNN 9.28.0 released on 2026-11-04; evaluate that package against ONNX Runtime compatibility, numerical behavior, and throughput before selecting a future runtime.
- **ONNX Runtime:** the package includes the MIT license and third-party notices. Confirm the exact GPU package dependency and bundled CUDA/cuDNN library mapping in the SBOM.
- **FFmpeg and codecs:** the candidate includes FFmpeg 8.1.3 shared libraries and notices for LGPL, OpenH264, libaom, and Kvazaar. Notices do not settle third-party codec patent or royalty obligations. NVIDIA's CUDA agreement also states that it grants no encoder/decoder patent rights.
- **NVENC:** the package includes the NVIDIA Video Codec SDK headers notice. Review the agreement and downstream terms for the exact headers/API version separately from CUDA runtime licensing.
- **Microsoft runtime:** the candidate includes Microsoft Visual C++ runtime files and a notice. Confirm the exact source, files, and deployment terms against Microsoft's [redistribution guidance](https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files).
- **Native package code:** the statically linked miniz 3.1.2 code is pinned to commit `77d0dce8627735138c51770d1799a1ef48f2117d`; the package includes its MIT notice.

## Release gates

1. Produce an SBOM for the exact signed installer/archive and bind every DLL to its package path, SHA-256, upstream source, version, license, and applicable redistribution clause or manifest.
2. Confirm every CUDA/cuDNN file in the selected profile is distributable under the exact source terms. cuDNN 9.8.0.87's eight DLL hashes and canonical license are now pinned and checked during packaging; this verifies origin and file identity, not legal eligibility. Keep runtime files in an application-private directory and make downstream terms satisfy the vendor agreements.
3. Complete independent FFmpeg codec-patent and NVENC agreement review; include required notices and terms.
4. Confirm Microsoft runtime redistribution for the exact files and install layout.
5. Build signed installer and portable packages; qualify clean-machine install, model acquisition/verification, GPU inference, export, update/rollback, uninstall, security, and the supported-device matrix.
6. For model downloads, sign the catalog with the publisher trust root, publish the exact model ZIP to at least two independent hosts, then anonymously download each copy and verify byte count and SHA-256 against the signed catalog. The RIFE 4.25 MIT scope is documented above; publisher authentication and channel read-back are not yet complete.

Keep public application downloads disabled until the applicable gates pass. The native Win32 desktop-only boundary remains in force.

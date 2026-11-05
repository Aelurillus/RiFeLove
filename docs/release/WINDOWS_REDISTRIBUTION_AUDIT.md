# Windows redistribution audit

**Status: public distribution is not qualified.** This is an engineering
inventory of one local package candidate, not a legal opinion or a grant of
redistribution rights. Do not use the candidate as a public release until the
open gates below are resolved against the exact files and terms shipped.

## Evidence snapshot

The reviewed local artifact is
`RifeLoveNative-local-release-candidate-rife-only-2026-11-06`. Its
`PACKAGE-MANIFEST.json` was generated on 2026-11-05 and has SHA-256
`9f40a5ae45a1cf891d2606f88e1192b56fd653d8fb82c47f53a33b1ee9a27fea`;
the executable hash is
`051dfbd6c4adf1566aba8e335b2072696e4a7b65b41dce621c8a169800a2423a`.
The manifest identifies a native Win32 package with `qtIncluded: false`,
`cliIncluded: false`, and `modelBundleMode: rife-only`. The included model
still says `redistribution: local-only-until-license-review`.

The package manifest lists 22 inference-runtime DLLs:

- CUDA math/runtime: `cublas64_12.dll`, `cublasLt64_12.dll`,
  `cudart64_12.dll`, `cufft64_11.dll`, `cufftw64_11.dll`, and
  `nvblas64_12.dll`.
- cuDNN 9: `cudnn64_9.dll`, `cudnn_adv64_9.dll`, `cudnn_cnn64_9.dll`,
  `cudnn_engines_precompiled64_9.dll`,
  `cudnn_engines_runtime_compiled64_9.dll`, `cudnn_graph64_9.dll`,
  `cudnn_heuristic64_9.dll`, and `cudnn_ops64_9.dll`.
- TensorRT 10.9.0.34: `nvinfer_10.dll`,
  `nvinfer_builder_resource_10.dll`, `nvinfer_plugin_10.dll`, and
  `nvonnxparser_10.dll`.
- ONNX Runtime: `onnxruntime.dll`, `onnxruntime_providers_shared.dll`,
  `onnxruntime_providers_cuda.dll`, and
  `onnxruntime_providers_tensorrt.dll`.

The same manifest records FFmpeg 8.1.3 libraries, including OpenH264,
libaom, and Kvazaar. The archive also contains compiler/runtime files,
Microsoft Visual C++ runtime DLLs, and 19 license/notice files. The native
installer now statically links miniz 3.1.2 for verified ZIP package handling;
future packages must include its MIT license and source commit in the manifest.
Those notices
are useful evidence of what was bundled; their presence alone does not prove
that each file may be redistributed in this package.

## License and distribution findings

- NVIDIA's [CUDA SDK agreement](https://docs.nvidia.com/cuda/eula/index.html)
  limits redistribution to components identified as distributable, requires
  material application functionality, app-scoped access, and distribution
  terms that protect NVIDIA's rights. Its CUDA supplement assigns separate
  third-party encoder/decoder license responsibility to the distributor.
  It scopes the SDK to applications for systems with NVIDIA GPUs. The exact
  source archive, version, file hash, and applicable attachment must be
  recorded for every CUDA-family DLL, and the product needs a GPU-runtime
  install boundary or a separately qualified CPU-only package.
- The [NVIDIA Video Codec SDK agreement](https://developer.nvidia.com/nvidia-video-codec-sdk-license-agreement)
  permits object-code distribution only as integrated in a licensee product
  for supported NVIDIA GPUs and requires enforceable downstream terms at
  least as restrictive as that agreement. The package includes an NV Codec
  headers notice; it does not include the downstream agreement or prove that
  the app's current end-user terms satisfy this requirement.
- TensorRT has a separate agreement. The bundle contains a copy named
  `TensorRT-10.9-LICENSE.txt` and records version `10.9.0.34`, but the four
  TensorRT DLLs still need a file-by-file match to the exact SDK distribution
  terms and source package. The multi-gigabyte builder resource must be
  included only if the selected public profile actually needs it and its
  distribution is confirmed.
- NVIDIA's cuDNN Windows installation material publishes release-specific
  redistribution manifests with component names, license names, URLs, and
  checksums. The package currently has a cuDNN license text, but the audit
  must bind the eight shipped DLLs to the exact `9.8.0.87` source manifest and
  license.
- Microsoft states that Visual C++ runtime redistribution is limited to
  licensed Visual Studio users and is governed by the applicable Microsoft
  Software License Terms. The package script itself labels this runtime as
  local-only pending review. Confirm the exact source and terms for the four
  packaged runtime DLLs before enabling public distribution.
- The package includes the RIFE MIT notice and a pinned Practical-RIFE source
  manifest, while its model entry remains explicitly marked local-only. The
  model hash is an integrity pin, not an author signature. Record the source
  provenance and the product's redistribution decision before advertising or
  hosting this model pack.

## Required release gates

1. Generate an SBOM for the exact release archive. For every DLL, bind its
   package path and SHA-256 to the upstream package, version, license text,
   and the exact clause or redistributable manifest that permits distribution.
2. Confirm each CUDA, cuDNN, TensorRT, and Video Codec component against the
   agreement version accepted for the source SDK. Keep SDK builders and other
   development-only files out unless the selected product profile requires
   them and their distribution is explicitly supported.
3. Review product terms and installer behavior against NVIDIA's downstream
   conditions, including application-private runtime placement and any
   required user acceptance. The repository's MIT source license is not
   evidence that these additional terms have been met.
4. Review third-party codec patent and royalty obligations independently of
   the included FFmpeg/OpenH264/Kvazaar/libaom license notices.
5. Confirm Microsoft runtime redistribution rights for the exact files and
   deployment layout, then include the required Microsoft notices.
6. Rebuild and re-audit the exact signed candidate, then complete clean-
   machine install, model import/download, inference, export, update/rollback,
   uninstall, security, and support-matrix acceptance.

Until these gates pass, keep `downloadAvailable` false and use the package
only for local qualification. The native-only/no-Qt/no-CLI product boundary
is retained; resolving the runtime audit does not by itself complete model
hosting or the in-app download/install workflow.

## Source references

- [NVIDIA CUDA SDK EULA and CUDA Toolkit supplement](https://docs.nvidia.com/cuda/eula/index.html)
- [NVIDIA Video Codec SDK license agreement](https://developer.nvidia.com/nvidia-video-codec-sdk-license-agreement)
- [NVIDIA cuDNN Windows installation and release manifests](https://docs.nvidia.com/cudnn/latest/installation/backend/windows/)
- [Microsoft: Redistribute Visual C++ files](https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files)
- The exact TensorRT 10.9 license copied inside the candidate archive: `licenses/TensorRT-10.9-LICENSE.txt`.

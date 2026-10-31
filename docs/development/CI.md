# Windows continuous integration

The `Windows core and native controls` workflow runs on pushes to `main`, pull
requests and manual dispatch. It builds the C++20 core and the native Win32
shell with the Windows runner's GCC toolchain, then runs the CPU and native
control tests. CMake downloads the pinned development dependencies and verifies
the SQLite archive hash. The workflow has read-only repository permissions and
does not publish executables, model weights or user data.

A separate CPU-only development-tools job checks tiling geometry, bounded
output lifetime, and model-pack evidence validation with synthetic fixtures.
Tests that require the local STDF graph or its private evaluation evidence are
skipped when those files are absent; no model weights are downloaded by CI.

The hosted run deliberately omits FFmpeg, ONNX Runtime CUDA, TensorRT and GPU
hardware tests. Its native executable is an inspection shell, not the complete
processing product. Green CI therefore does not validate encoding, model
correctness, CUDA, TensorRT, real video processing, or release packaging; those
continue to require the local SDK and NVIDIA test matrix documented elsewhere.

#pragma once

#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>

#include <filesystem>
#include <memory>
#include <string>

namespace rifelove::inference {

// Internal process-wide lease over the existing ONNX Runtime loader. The owner
// keeps the already-created Runtime/OrtEnv and all provider DLLs alive. This is
// deliberately smaller than a public backend API: session and provider-option
// ownership remains with each backend.
struct OrtRuntimeLease final {
    std::shared_ptr<void> owner;
    const OrtApi* api = nullptr;
    OrtEnv* environment = nullptr;
    std::filesystem::path directory;
    std::string version;
    bool cudaProviderAvailable = false;
};

[[nodiscard]] OrtRuntimeLease acquireOrtRuntimeLease(const std::filesystem::path& directory);

} // namespace rifelove::inference

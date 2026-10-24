#pragma once

#include "gpu/cuda_device.hpp"
#include "inference/model_pack.hpp"
#include <cstddef>
#include <string>

namespace rifelove::inference {

struct TensorRtCacheIdentity {
    std::string onnxRuntimeVersion;
    std::string tensorRtVersion;
    std::string cudaRuntimeVersion;
    std::string precision;
    std::string shapeProfile;
    std::size_t workspaceBytes;
};

[[nodiscard]] std::string tensorRtEngineCachePrefix(const ModelPack& model, const gpu::DeviceInfo& device,
    const TensorRtCacheIdentity& identity);

} // namespace rifelove::inference

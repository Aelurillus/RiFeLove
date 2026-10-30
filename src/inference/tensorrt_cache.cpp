#include "inference/tensorrt_cache.hpp"

#include <span>
#include <string_view>

namespace rifelove::inference {
std::string tensorRtEngineCachePrefix(const ModelPack& model, const gpu::DeviceInfo& device,
    const TensorRtCacheIdentity& identity) {
    return tensorRtEngineCachePrefix("rife",model.id,model.version,model.sha256,device,identity);
}
std::string tensorRtEngineCachePrefix(std::string_view pipeline, std::string_view modelId,
    std::string_view modelVersion, std::string_view modelSha256, const gpu::DeviceInfo& device,
    const TensorRtCacheIdentity& identity) {
    std::string material;
    const auto field = [&](std::string_view value) {
        material.append(std::to_string(value.size()));
        material.push_back(':');
        material.append(value);
        material.push_back('|');
    };
    // Keep the existing RIFE cache identity byte-for-byte stable. New pipeline
    // families add their namespace to prevent cross-stage engine reuse.
    field(pipeline == "rife" ? "rifelove-tensorrt-cache-v2" : "rifelove-tensorrt-cache-v3");
    if (pipeline != "rife") field(pipeline);
    field(modelId); field(modelVersion); field(modelSha256);
    field(identity.onnxRuntimeVersion); field(identity.tensorRtVersion); field(identity.cudaRuntimeVersion);
    field(identity.precision); field(identity.shapeProfile);
    field(device.name); field(device.pciBusId);
    field(std::to_string(device.ordinal)); field(std::to_string(device.computeMajor)); field(std::to_string(device.computeMinor));
    field(std::to_string(device.driverCudaApiVersion));
    field(std::to_string(identity.workspaceBytes));
    const auto bytes = std::as_bytes(std::span(material.data(), material.size()));
    return std::string(pipeline) + "-" + sha256(bytes);
}
} // namespace rifelove::inference

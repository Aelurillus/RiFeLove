#include "inference/tensorrt_cache.hpp"

#include <span>
#include <string_view>

namespace rifelove::inference {
std::string tensorRtEngineCachePrefix(const ModelPack& model, const gpu::DeviceInfo& device,
    const TensorRtCacheIdentity& identity) {
    std::string material;
    const auto field = [&](std::string_view value) {
        material.append(std::to_string(value.size()));
        material.push_back(':');
        material.append(value);
        material.push_back('|');
    };
    field("rifelove-tensorrt-cache-v2");
    field(model.id); field(model.version); field(model.sha256);
    field(identity.onnxRuntimeVersion); field(identity.tensorRtVersion); field(identity.cudaRuntimeVersion);
    field(identity.precision); field(identity.shapeProfile);
    field(device.name); field(device.pciBusId);
    field(std::to_string(device.ordinal)); field(std::to_string(device.computeMajor)); field(std::to_string(device.computeMinor));
    field(std::to_string(device.driverCudaApiVersion));
    field(std::to_string(identity.workspaceBytes));
    const auto bytes = std::as_bytes(std::span(material.data(), material.size()));
    return "rife-" + sha256(bytes);
}
} // namespace rifelove::inference

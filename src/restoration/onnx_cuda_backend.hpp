#pragma once

#include "restoration/model_pack.hpp"
#include "inference/onnx_cuda_backend.hpp"

#include <memory>

namespace rifelove::gpu { class CudaDevice; }

namespace rifelove::restoration {

struct OnnxCudaRestorationConfig {
    std::filesystem::path runtimeDirectory;
    std::filesystem::path engineCacheDirectory;
    std::filesystem::path profilePrefix; // Empty disables developer profiling.
    std::size_t arenaBudgetBytes = 1024ULL * 1024 * 1024;
    std::size_t hostFloatBufferLimit = 512ULL * 1024 * 1024;
    std::size_t maxQueuedRequests = 1;
    int tileSize = 256;
    int tileOverlap = 32;
    inference::ProviderPreference providerPreference = inference::ProviderPreference::preferTensorRt;
};

// Runs SDR restoration models on the selected CUDA device. Image adapters use
// NCHW RGB. The explicitly named FastDVDnet adapter uses five real RGB frames
// plus a Gaussian-noise map, packed as 16 channels, and preserves center PTS.
class OnnxCudaVideoRestorationBackend final : public IVideoRestorationBackend {
public:
    OnnxCudaVideoRestorationBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaRestorationConfig config);
    ~OnnxCudaVideoRestorationBackend() override;
    OnnxCudaVideoRestorationBackend(const OnnxCudaVideoRestorationBackend&) = delete;
    OnnxCudaVideoRestorationBackend& operator=(const OnnxCudaVideoRestorationBackend&) = delete;

    [[nodiscard]] Capabilities capabilities() const override;
    void loadModel(const ModelPack& model);
    [[nodiscard]] std::future<Result> restore(Request request) override;
    [[nodiscard]] std::filesystem::path finishProfiling();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rifelove::restoration

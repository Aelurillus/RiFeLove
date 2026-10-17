#pragma once

#include "inference/interpolation_backend.hpp"

namespace rifelove::inference {
struct OnnxCudaConfig {
    std::filesystem::path runtimeDirectory; // Explicit directory containing reviewed DLLs.
    std::size_t arenaBudgetBytes = 1024ULL * 1024 * 1024; // ORT arena only, not total VRAM.
    std::size_t maxQueuedRequests = 2;
    std::filesystem::path profilePrefix; // Empty disables profiling.
};
class OnnxCudaBackend final : public IInterpolationBackend {
public:
    OnnxCudaBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaConfig config);
    ~OnnxCudaBackend() override;
    [[nodiscard]] ModelCapabilities capabilities() const override;
    void loadModel(const ModelPack&) override;
    [[nodiscard]] std::future<InterpolationResult> interpolate(InterpolationRequest request) override;
    // A one-shot diagnostic. The backend must be idle; this stops session profiling.
    [[nodiscard]] std::filesystem::path finishProfiling();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rifelove::inference

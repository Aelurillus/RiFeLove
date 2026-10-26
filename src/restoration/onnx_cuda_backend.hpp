#pragma once

#include "restoration/model_pack.hpp"

#include <memory>

namespace rifelove::gpu { class CudaDevice; }

namespace rifelove::restoration {

struct OnnxCudaRestorationConfig {
    std::filesystem::path runtimeDirectory;
    std::size_t arenaBudgetBytes = 1024ULL * 1024 * 1024;
    std::size_t hostFloatBufferLimit = 512ULL * 1024 * 1024;
    std::size_t maxQueuedRequests = 1;
    int tileSize = 256;
    int tileOverlap = 32;
};

// Runs one-frame SDR image restoration ONNX models on the selected CUDA device.
// The graph adapter contract is one FP32 NCHW RGB tensor in [0,1] and one
// same-layout output tensor; temporal video models require a separate adapter.
class OnnxCudaVideoRestorationBackend final : public IVideoRestorationBackend {
public:
    OnnxCudaVideoRestorationBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaRestorationConfig config);
    ~OnnxCudaVideoRestorationBackend() override;
    OnnxCudaVideoRestorationBackend(const OnnxCudaVideoRestorationBackend&) = delete;
    OnnxCudaVideoRestorationBackend& operator=(const OnnxCudaVideoRestorationBackend&) = delete;

    [[nodiscard]] Capabilities capabilities() const override;
    void loadModel(const ModelPack& model);
    [[nodiscard]] std::future<Result> restore(Request request) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rifelove::restoration

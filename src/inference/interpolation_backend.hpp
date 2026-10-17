#pragma once

#include "gpu/cuda_device.hpp"
#include "inference/model_pack.hpp"
#include <future>

namespace rifelove::inference {

enum class InferenceErrorKind { unavailable, invalidRequest, outOfMemory, cancelled, busy, runtimeError };
class InferenceError : public std::runtime_error {
public:
    InferenceError(InferenceErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    [[nodiscard]] InferenceErrorKind kind() const noexcept { return kind_; }
private:
    InferenceErrorKind kind_;
};
struct ModelCapabilities { std::string backend; std::string modelId; int spatialMultiple; bool dynamicTimestep; bool fp16; };
// Immutable through this interface. The uploader must relinquish mutable aliases
// before submitting a frame, and keep its data unchanged until its future ends.
struct GpuFrame { std::shared_ptr<const gpu::DeviceBuffer> pixels; int width; int height; };
struct InterpolationRequest {
    GpuFrame frame0;
    GpuFrame frame1;
    float timestep;
    // Optional exclusive reusable output. Move the only mutable owner into the
    // request. The backend rejects shared/aliased output storage.
    std::shared_ptr<gpu::DeviceBuffer> output;
    std::stop_token stop;
};
struct InterpolationResult { GpuFrame frame; double elapsedSeconds; bool reusedOutput; };
class IInterpolationBackend {
public:
    virtual ~IInterpolationBackend() = default;
    [[nodiscard]] virtual ModelCapabilities capabilities() const = 0;
    virtual void loadModel(const ModelPack&) = 0;
    [[nodiscard]] virtual std::future<InterpolationResult> interpolate(InterpolationRequest request) = 0;
};

} // namespace rifelove::inference

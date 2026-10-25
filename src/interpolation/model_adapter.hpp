#pragma once

#include "inference/interpolation_backend.hpp"
#include "media/rife_color.hpp"
#include "interpolation/scene_analysis.hpp"

#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace rifelove::interpolation {

// Adapters are application-owned code selected by a model pack's adapter ID.
// They convert decoded media to the backend's canonical FP32 NCHW RGB tensors,
// expose a quality-analysis view, and restore generated tensors to media frames.
// Model packs never execute code and cannot register adapters by themselves.
class IModelAdapter {
public:
    virtual ~IModelAdapter() = default;
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual int version() const noexcept = 0;
    [[nodiscard]] virtual bool supports(const inference::ModelCapabilities& model) const noexcept = 0;
    [[nodiscard]] virtual std::string inputProfile(const media::ColorInfo& color) const = 0;
    [[nodiscard]] virtual media::RifeSdrFrame preprocess(const media::DecodedFrame& frame,
        std::size_t hostFloatBufferLimit, std::stop_token stop) const = 0;
    [[nodiscard]] virtual quality::RgbView analysisView(const media::RifeSdrFrame& frame) const = 0;
    [[nodiscard]] virtual media::DecodedFrame postprocess(const media::RifeSdrFrame& frame,
        std::stop_token stop) const = 0;
};

class ModelAdapterRegistry final {
public:
    void registerAdapter(std::shared_ptr<const IModelAdapter> adapter);
    [[nodiscard]] const IModelAdapter& resolve(const inference::ModelCapabilities& model) const;
    [[nodiscard]] std::size_t size() const noexcept { return adapters_.size(); }
private:
    using Key = std::pair<std::string,int>;
    std::map<Key,std::shared_ptr<const IModelAdapter>> adapters_;
};

// Extend a fresh registry with the built-in adapter before adding project or
// application adapters. Freeze the registry before sharing it with a pipeline.
void registerBuiltInModelAdapters(ModelAdapterRegistry& registry);
[[nodiscard]] std::shared_ptr<const ModelAdapterRegistry> defaultModelAdapterRegistry();

class RifeSdrModelAdapter final : public IModelAdapter {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "rife-sdr-v1"; }
    [[nodiscard]] int version() const noexcept override { return 1; }
    [[nodiscard]] bool supports(const inference::ModelCapabilities& model) const noexcept override;
    [[nodiscard]] std::string inputProfile(const media::ColorInfo& color) const override;
    [[nodiscard]] media::RifeSdrFrame preprocess(const media::DecodedFrame& frame,
        std::size_t hostFloatBufferLimit, std::stop_token stop) const override;
    [[nodiscard]] quality::RgbView analysisView(const media::RifeSdrFrame& frame) const override;
    [[nodiscard]] media::DecodedFrame postprocess(const media::RifeSdrFrame& frame,
        std::stop_token stop) const override;
};

} // namespace rifelove::interpolation

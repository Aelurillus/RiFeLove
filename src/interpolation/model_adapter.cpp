#include "interpolation/model_adapter.hpp"

#include <stdexcept>

namespace rifelove::interpolation {

void ModelAdapterRegistry::registerAdapter(std::shared_ptr<const IModelAdapter> adapter) {
    if (!adapter) throw std::invalid_argument("Cannot register an empty model adapter");
    const auto adapterId = adapter->id();
    if (adapterId.empty() || adapterId.size() > 128 || adapter->version() <= 0)
        throw std::invalid_argument("Model adapter has an invalid identity");
    const auto inserted = adapters_.emplace(Key{std::string(adapterId),adapter->version()},std::move(adapter)).second;
    if (!inserted) throw std::invalid_argument("Model adapter identity is already registered");
}

const IModelAdapter& ModelAdapterRegistry::resolve(const inference::ModelCapabilities& model) const {
    const auto found = adapters_.find(Key{model.adapterId,model.adapterVersion});
    if (found == adapters_.end())
        throw std::invalid_argument("No installed model adapter matches " + model.adapterId + " version " + std::to_string(model.adapterVersion));
    if (!found->second->supports(model))
        throw std::invalid_argument("Installed model adapter does not support the verified model tensor contract");
    return *found->second;
}

void registerBuiltInModelAdapters(ModelAdapterRegistry& registry) {
    registry.registerAdapter(std::make_shared<RifeSdrModelAdapter>());
}

std::shared_ptr<const ModelAdapterRegistry> defaultModelAdapterRegistry() {
    static const auto registry = [] {
        auto value = std::make_shared<ModelAdapterRegistry>();
        registerBuiltInModelAdapters(*value);
        return std::shared_ptr<const ModelAdapterRegistry>(std::move(value));
    }();
    return registry;
}

bool RifeSdrModelAdapter::supports(const inference::ModelCapabilities& model) const noexcept {
    return model.adapterId == id() && model.adapterVersion == version() && model.spatialMultiple == 64 &&
        model.dynamicTimestep && !model.modelId.empty() && !model.supportedInputProfiles.empty();
}

std::string RifeSdrModelAdapter::inputProfile(const media::ColorInfo& color) const {
    return media::rifeSdrInputProfile(color);
}

media::RifeSdrFrame RifeSdrModelAdapter::preprocess(const media::DecodedFrame& frame,
    std::size_t hostFloatBufferLimit, std::stop_token stop) const {
    return media::prepareRifeSdr(frame,hostFloatBufferLimit,stop);
}

quality::RgbView RifeSdrModelAdapter::analysisView(const media::RifeSdrFrame& frame) const {
    const auto planeSize = static_cast<std::size_t>(frame.paddedWidth) * static_cast<std::size_t>(frame.paddedHeight);
    return {frame.pixels,frame.width,frame.height,frame.paddedWidth,planeSize};
}

media::DecodedFrame RifeSdrModelAdapter::postprocess(const media::RifeSdrFrame& frame, std::stop_token stop) const {
    return media::restoreRifeSdr(frame,stop);
}

} // namespace rifelove::interpolation

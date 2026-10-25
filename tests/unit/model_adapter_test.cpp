#include "interpolation/model_adapter.hpp"
#include <gtest/gtest.h>

using namespace rifelove;

namespace {
class CustomAdapter final : public interpolation::IModelAdapter {
public:
    std::string_view id() const noexcept override { return "test-custom"; }
    int version() const noexcept override { return 2; }
    bool supports(const inference::ModelCapabilities& model) const noexcept override {
        return model.adapterId == id() && model.adapterVersion == version() && model.spatialMultiple == 64 && model.dynamicTimestep;
    }
    std::string inputProfile(const media::ColorInfo& color) const override { return delegate_.inputProfile(color); }
    media::RifeSdrFrame preprocess(const media::DecodedFrame& frame, std::size_t limit, std::stop_token stop) const override {
        return delegate_.preprocess(frame,limit,stop);
    }
    quality::RgbView analysisView(const media::RifeSdrFrame& frame) const override { return delegate_.analysisView(frame); }
    media::DecodedFrame postprocess(const media::RifeSdrFrame& frame, std::stop_token stop) const override {
        return delegate_.postprocess(frame,stop);
    }
private:
    interpolation::RifeSdrModelAdapter delegate_;
};

inference::ModelCapabilities model(std::string adapterId = "rife-sdr-v1", int adapterVersion = 1) {
    inference::ModelCapabilities result;
    result.modelId = "fixture-rife";
    result.spatialMultiple = 64;
    result.dynamicTimestep = true;
    result.supportedInputProfiles = {"bt709-sdr8"};
    result.adapterId = std::move(adapterId);
    result.adapterVersion = adapterVersion;
    return result;
}
}

TEST(ModelAdapterRegistry, BuiltInRifeAdapterResolvesAndValidatesTheTensorContract) {
    const auto registry = interpolation::defaultModelAdapterRegistry();
    ASSERT_EQ(registry->size(),1U);
    const auto& adapter = registry->resolve(model());
    EXPECT_EQ(adapter.id(),"rife-sdr-v1");
    EXPECT_EQ(adapter.version(),1);
    auto unsupported = model();
    unsupported.spatialMultiple = 128;
    EXPECT_THROW((void)registry->resolve(unsupported),std::invalid_argument);
    EXPECT_THROW((void)registry->resolve(model("missing-adapter",1)),std::invalid_argument);
    EXPECT_THROW((void)registry->resolve(model("rife-sdr-v1",2)),std::invalid_argument);
}

TEST(ModelAdapterRegistry, CustomAdapterCanBeRegisteredAndSelectedByVersion) {
    interpolation::ModelAdapterRegistry registry;
    interpolation::registerBuiltInModelAdapters(registry);
    registry.registerAdapter(std::make_shared<CustomAdapter>());
    const auto& selected = registry.resolve(model("test-custom",2));
    EXPECT_EQ(selected.id(),"test-custom");
    EXPECT_EQ(selected.version(),2);
    EXPECT_THROW(registry.registerAdapter(std::make_shared<CustomAdapter>()),std::invalid_argument);
}

TEST(ModelAdapterRegistry, BuiltInAdapterOwnsTheSdrPreAndPostprocessingRoute) {
    const auto registry = interpolation::defaultModelAdapterRegistry();
    const auto& adapter = registry->resolve(model());
    media::DecodedFrame frame{};
    frame.width = 2;
    frame.height = 2;
    frame.color.pixelFormat = "rgb24";
    frame.color.bitDepth = 8;
    frame.color.primaries = "bt709";
    frame.color.transfer = "iec61966-2-1";
    frame.color.matrix = "gbr";
    frame.color.range = "pc";
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255,0,0,0,255,0,0,0,255,255,255,255});
    frame.planes[0] = {bytes->data(),6};
    frame.storage = bytes;
    EXPECT_EQ(adapter.inputProfile(frame.color),"bt709-sdr8");
    const auto prepared = adapter.preprocess(frame,512ULL * 1024 * 1024,{});
    const auto view = adapter.analysisView(prepared);
    EXPECT_EQ(view.width,2);
    EXPECT_EQ(view.height,2);
    EXPECT_EQ(view.rowStride,64);
    EXPECT_EQ(view.planeSize,4096U);
    const auto restored = adapter.postprocess(prepared,{});
    EXPECT_EQ(restored.width,2);
    EXPECT_EQ(restored.height,2);
    EXPECT_EQ(restored.color.matrix,"gbr");
    ASSERT_TRUE(restored.storage);
}

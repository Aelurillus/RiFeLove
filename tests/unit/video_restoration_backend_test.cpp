#include "restoration/video_restoration_backend.hpp"

#include <gtest/gtest.h>
#include <future>
#include <limits>

using namespace rifelove;

namespace {
restoration::Capabilities capabilities() {
    return {"basicvsrpp-reds4","basicvsrpp-onnx-v1",1,
        {restoration::Task::superResolution},
        {restoration::Placement::sourceRateBeforeInterpolation},
        {"bt709-sdr8"},1,1,2,1,true};
}

media::DecodedFrame frame(std::size_t index, Rational timestamp, int width = 64, int height = 64) {
    media::DecodedFrame result{};
    result.index = index;
    result.width = width;
    result.height = height;
    result.timestamp = timestamp;
    result.duration = Rational(1,30);
    result.color.pixelFormat = "rgb24";
    result.color.bitDepth = 8;
    result.color.primaries = "bt709";
    result.color.transfer = "iec61966-2-1";
    result.color.matrix = "gbr";
    result.color.range = "pc";
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3,127);
    result.planes[0] = {pixels->data(),width * 3};
    result.storage = std::move(pixels);
    return result;
}

restoration::Request request() {
    restoration::Request value;
    value.task = restoration::Task::superResolution;
    value.placement = restoration::Placement::sourceRateBeforeInterpolation;
    value.context = {frame(0,Rational(0)),frame(1,Rational(1,30)),frame(2,Rational(2,30))};
    value.centerIndex = 1;
    value.inputProfile = "bt709-sdr8";
    return value;
}

class FakeRestorationBackend final : public restoration::IVideoRestorationBackend {
public:
    restoration::Capabilities capabilities() const override { return ::capabilities(); }
    std::future<restoration::Result> restore(restoration::Request) override {
        return std::async(std::launch::deferred,[] { return restoration::Result{}; });
    }
};
}

TEST(VideoRestorationContract, ValidTemporalWindowPreservesPresentationColorAndScale) {
    const auto caps = capabilities();
    const auto input = request();
    EXPECT_NO_THROW(restoration::validateRequest(caps,input));
    restoration::Result output{frame(1,Rational(1,30),128,128),0.25};
    EXPECT_NO_THROW(restoration::validateResult(caps,input,output));
}

TEST(VideoRestorationContract, RecognizesOnlySupportedSingleFrameOutputRateTasks) {
    auto deblur = capabilities();
    deblur.modelId = "nafnet-gopro-deblur-sdr8";
    deblur.adapterId = "nafnet-zoo-deblur-rgb-f32-nchw-v1";
    deblur.tasks = {restoration::Task::deblur};
    deblur.placements = {restoration::Placement::outputRateAfterInterpolation};
    deblur.temporalFramesBefore = 0;
    deblur.temporalFramesAfter = 0;
    deblur.scaleNumerator = 1;
    deblur.scaleDenominator = 1;
    EXPECT_EQ(restoration::outputRateImageTask(deblur),restoration::Task::deblur);

    auto superResolution = deblur;
    superResolution.tasks = {restoration::Task::superResolution};
    superResolution.adapterId = "image-sr-rgb-f32-nchw-v1";
    superResolution.scaleNumerator = 4;
    EXPECT_EQ(restoration::outputRateImageTask(superResolution),restoration::Task::superResolution);

    deblur.tasks.push_back(restoration::Task::superResolution);
    EXPECT_FALSE(restoration::outputRateImageTask(deblur));
    deblur.tasks = {restoration::Task::deblur};
    deblur.placements = {restoration::Placement::sourceRateBeforeInterpolation};
    EXPECT_FALSE(restoration::outputRateImageTask(deblur));
    deblur.placements = {restoration::Placement::outputRateAfterInterpolation};
    deblur.temporalFramesAfter = 1;
    EXPECT_FALSE(restoration::outputRateImageTask(deblur));
}

TEST(VideoRestorationContract, RejectsUnsupportedTaskPlacementAndColorProfile) {
    const auto caps = capabilities();
    auto input = request();
    input.task = restoration::Task::denoise;
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.placement = restoration::Placement::outputRateAfterInterpolation;
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.inputProfile = "pq-hdr10";
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.inputProfile = "bt470bg-sdr8";
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.context[1].color.transfer = "smpte2084";
    input.context[1].color.hdrTransfer = true;
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
}

TEST(VideoRestorationContract, RequiresAnOrderedUniformTimestampedColorStableWindow) {
    const auto caps = capabilities();
    auto input = request();
    input.context[2].timestamp = Rational(1,30);
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.context[1].width = 128;
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.context[1].color.transfer = "smpte2084";
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
    input = request();
    input.context[1].timestamp.reset();
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
}

TEST(VideoRestorationContract, BoundaryPaddingMustBeDeclaredAndPreservesRealFramePts) {
    auto caps = capabilities();
    auto input = request();
    input.context.erase(input.context.begin());
    input.centerIndex = 0;
    input.missingFramesBefore = 1;
    EXPECT_NO_THROW(restoration::validateRequest(caps,input));
    caps.supportsBoundaryPadding = false;
    EXPECT_THROW(restoration::validateRequest(caps,input),std::invalid_argument);
}

TEST(VideoRestorationContract, RejectsShiftedTimesWrongScaleColorAndInvalidTiming) {
    const auto caps = capabilities();
    const auto input = request();
    auto output = restoration::Result{frame(1,Rational(4,30),128,128),0.25};
    EXPECT_THROW(restoration::validateResult(caps,input,output),std::invalid_argument);
    output = restoration::Result{frame(2,Rational(1,30),128,128),0.25};
    EXPECT_THROW(restoration::validateResult(caps,input,output),std::invalid_argument);
    output = restoration::Result{frame(1,Rational(1,30),127,128),0.25};
    EXPECT_THROW(restoration::validateResult(caps,input,output),std::invalid_argument);
    output = restoration::Result{frame(1,Rational(1,30),128,128),std::numeric_limits<double>::quiet_NaN()};
    EXPECT_THROW(restoration::validateResult(caps,input,output),std::invalid_argument);
    output = restoration::Result{frame(1,Rational(1,30),128,128),0.25};
    output.centerFrame.color.transfer = "smpte2084";
    EXPECT_THROW(restoration::validateResult(caps,input,output),std::invalid_argument);
}

TEST(VideoRestorationRegistry, RegistersAndResolvesByVersionedModelIdentity) {
    restoration::Registry registry;
    const auto backend = std::make_shared<FakeRestorationBackend>();
    registry.registerBackend(backend);
    EXPECT_EQ(registry.size(),1U);
    EXPECT_EQ(registry.resolve("basicvsrpp-reds4","basicvsrpp-onnx-v1",1),backend);
    EXPECT_THROW((void)registry.resolve("basicvsrpp-reds4","basicvsrpp-onnx-v1",2),std::invalid_argument);
    EXPECT_THROW(registry.registerBackend(backend),std::invalid_argument);
    EXPECT_THROW(registry.registerBackend(nullptr),std::invalid_argument);
}

TEST(VideoRestorationRegistry, RejectsUnsafeOrAmbiguousModelCapabilities) {
    restoration::Registry registry;
    auto invalid = capabilities();
    invalid.modelId = "../weights/model";
    class InvalidBackend final : public restoration::IVideoRestorationBackend {
    public:
        explicit InvalidBackend(restoration::Capabilities value) : value_(std::move(value)) {}
        restoration::Capabilities capabilities() const override { return value_; }
        std::future<restoration::Result> restore(restoration::Request) override {
            return std::async(std::launch::deferred,[] { return restoration::Result{}; });
        }
    private:
        restoration::Capabilities value_;
    };
    EXPECT_THROW(registry.registerBackend(std::make_shared<InvalidBackend>(invalid)),std::invalid_argument);
    invalid = capabilities();
    invalid.supportedInputProfiles.push_back("bt709-sdr8");
    EXPECT_THROW(registry.registerBackend(std::make_shared<InvalidBackend>(invalid)),std::invalid_argument);
}

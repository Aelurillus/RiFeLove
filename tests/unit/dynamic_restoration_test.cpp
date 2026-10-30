#include "restoration/dynamic_control.hpp"
#include "restoration/temporal_window.hpp"
#include "restoration/noise_estimate.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <random>

using namespace rifelove;
using namespace rifelove::restoration;

namespace {
Capabilities temporalCaps() {
    return {"five-frame-eval","fastdvdnet-rgb-f32-nchw16-v1",1,{Task::denoise},
        {Placement::sourceRateBeforeInterpolation},{"bt709-sdr8"},2,2,1,1,true};
}
media::DecodedFrame frame(std::size_t index) {
    media::DecodedFrame value{};
    value.index = index; value.width = 16; value.height = 16;
    value.timestamp = Rational(static_cast<std::int64_t>(index * index + index),1000);
    value.duration = Rational(1,30);
    value.color.pixelFormat = "rgb24"; value.color.bitDepth = 8; value.color.primaries = "bt709";
    value.color.transfer = "iec61966-2-1"; value.color.matrix = "gbr"; value.color.range = "pc";
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(16 * 16 * 3,127);
    value.planes[0] = {pixels->data(),16 * 3}; value.storage = pixels;
    return value;
}
DynamicObservation observation(Rational pts, double automatic, double confidence = 1.0, bool cut = false) {
    DynamicObservation result{pts,{},cut};
    result.effects.fill({automatic,confidence});
    return result;
}
}

TEST(DynamicRestoration, RelativeControlsPreserveAutomaticAndHaveExactEndpoints) {
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,0),.2);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,-1),0);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,1),1);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,-.5),.1);
    EXPECT_NEAR(DynamicController::relativeStrength(.2,.5),.6,1e-15);
    EXPECT_THROW((void)DynamicController::relativeStrength(.2,2),std::invalid_argument);
    EXPECT_THROW((void)DynamicController::relativeStrength(-.1,0),std::invalid_argument);
}

TEST(DynamicRestoration, ThreeSettingsAreIndependentAndMissingEstimatesStayDisabled) {
    DynamicSettings settings;
    settings.effects[0].relative = -.5;
    settings.effects[1].relative = .5;
    settings.effects[2].enabled = false;
    DynamicController controller(settings,"model-and-settings-v1");
    const auto result = controller.update(observation(0,.2));
    EXPECT_DOUBLE_EQ(result.strength[0],.1);
    EXPECT_NEAR(result.strength[1],.6,1e-15);
    EXPECT_DOUBLE_EQ(result.strength[2],0);
    DynamicController uncertain(settings,"model-and-settings-v1");
    const auto absent = uncertain.update(observation(0,.2,0));
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_FALSE(absent.hasEstimate[index]);
        EXPECT_DOUBLE_EQ(absent.strength[index],0);
    }
}

TEST(DynamicRestoration, PtsBasedSmoothingAndSlewLimitPreventFrameToFrameJumps) {
    DynamicSettings settings;
    settings.maximumChangePerSecond = .3;
    DynamicController controller(settings,"identity");
    (void)controller.update(observation(0,.1));
    const auto next = controller.update(observation(Rational(1,30),.9));
    EXPECT_LE(next.automatic[0],.11000000000001);
    EXPECT_GT(next.automatic[0],.1);
    const auto held = controller.update(observation(Rational(2,30),.0,.1));
    EXPECT_DOUBLE_EQ(next.automatic[0],held.automatic[0]);
}

TEST(DynamicRestoration, SceneCutsResetEveryEffectWithoutLeakingPreviousScene) {
    DynamicController controller({},"identity");
    (void)controller.update(observation(0,.9));
    auto cut = observation(Rational(1,30),.1,1,true);
    cut.effects[1].confidence = 0;
    const auto result = controller.update(cut);
    EXPECT_DOUBLE_EQ(result.automatic[0],.1);
    EXPECT_DOUBLE_EQ(result.automatic[2],.1);
    EXPECT_FALSE(result.hasEstimate[1]);
    EXPECT_DOUBLE_EQ(result.strength[1],0);
    EXPECT_EQ(controller.checkpoint().sceneSerial,1U);
}

TEST(DynamicRestoration, CheckpointResumeExactlyReproducesContinuousControl) {
    DynamicController continuous({},"hash-model-analyzer-settings");
    for (int index = 0; index < 17; ++index)
        (void)continuous.update(observation(Rational(index,30),index % 2 ? .1 : .8));
    DynamicController resumed({},"hash-model-analyzer-settings");
    resumed.restore(continuous.checkpoint());
    for (int index = 17; index < 50; ++index) {
        const auto input = observation(Rational(index,30),index % 2 ? .8 : .1,1,index == 31);
        const auto a = continuous.update(input), b = resumed.update(input);
        EXPECT_EQ(a.automatic,b.automatic);
        EXPECT_EQ(a.strength,b.strength);
        EXPECT_EQ(a.hasEstimate,b.hasEstimate);
    }
}

TEST(DynamicRestoration, RejectsWrongIdentityInvalidStateAndUnorderedPtsWithoutMutation) {
    DynamicController controller({},"identity");
    (void)controller.update(observation(1,.5));
    const auto original = controller.checkpoint();
    EXPECT_THROW((void)controller.update(observation(1,.6)),std::invalid_argument);
    auto invalid = observation(2,.3); invalid.effects[2].confidence = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)controller.update(invalid),std::invalid_argument);
    auto wrong = original; wrong.identity = "other-model";
    EXPECT_THROW(controller.restore(wrong),std::invalid_argument);
    wrong = original; wrong.automatic[1] = 2;
    EXPECT_THROW(controller.restore(wrong),std::invalid_argument);
    EXPECT_EQ(controller.checkpoint().automatic,original.automatic);
    EXPECT_EQ(controller.checkpoint().lastTimestamp,original.lastTimestamp);
}

TEST(TemporalRestorationWindow, DeliversEveryRealVfrFrameInOrderWithBoundedMemory) {
    TemporalWindow window(temporalCaps());
    std::vector<Request> requests;
    for (std::size_t index = 0; index < 50; ++index) {
        auto ready = window.push(frame(index));
        requests.insert(requests.end(),ready.begin(),ready.end());
        EXPECT_LE(window.bufferedFrames(),4U);
    }
    auto end = window.finish(); requests.insert(requests.end(),end.begin(),end.end());
    ASSERT_EQ(requests.size(),50U);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        EXPECT_EQ(requests[index].context[requests[index].centerIndex].index,index);
        EXPECT_EQ(requests[index].context[requests[index].centerIndex].timestamp,frame(index).timestamp);
        EXPECT_NO_THROW(validateRequest(temporalCaps(),requests[index]));
    }
    EXPECT_EQ(requests[0].missingFramesBefore,2U);
    EXPECT_EQ(requests[1].missingFramesBefore,1U);
    EXPECT_EQ(requests[48].missingFramesAfter,1U);
    EXPECT_EQ(requests[49].missingFramesAfter,2U);
    EXPECT_TRUE(window.finish().empty());
    EXPECT_THROW((void)window.push(frame(50)),std::invalid_argument);
}

TEST(TemporalRestorationWindow, NeverIncludesTheOtherSceneEvenForOneFrameClips) {
    TemporalWindow window(temporalCaps());
    std::vector<Request> requests;
    for (std::size_t index = 0; index < 9; ++index) {
        auto ready = window.push(frame(index),index == 1 || index == 3 || index == 7);
        requests.insert(requests.end(),ready.begin(),ready.end());
    }
    auto end = window.finish(); requests.insert(requests.end(),end.begin(),end.end());
    ASSERT_EQ(requests.size(),9U);
    auto scene = [](std::size_t index) { return index < 1 ? 0 : index < 3 ? 1 : index < 7 ? 2 : 3; };
    for (const auto& request : requests)
        for (const auto& context : request.context)
            EXPECT_EQ(scene(context.index),scene(request.context[request.centerIndex].index));
    EXPECT_EQ(requests[0].context.size(),1U);
    EXPECT_EQ(requests[0].missingFramesBefore,2U);
    EXPECT_EQ(requests[0].missingFramesAfter,2U);
}

TEST(TemporalRestorationWindow, RejectsBadInputBeforeChangingThePendingWindow) {
    TemporalWindow window(temporalCaps());
    EXPECT_TRUE(window.push(frame(0)).empty());
    EXPECT_THROW((void)window.push(frame(0)),std::invalid_argument);
    auto wrong = frame(1); wrong.width = 32;
    EXPECT_THROW((void)window.push(wrong),std::invalid_argument);
    EXPECT_EQ(window.bufferedFrames(),1U);
    EXPECT_TRUE(window.push(frame(1)).empty());
    EXPECT_EQ(window.push(frame(2)).size(),1U);
}

namespace {
media::RifeSdrFrame noiseFrame(int width, int height, double sigma255 = 0.0) {
    const int paddedWidth = ((width + 63) / 64) * 64, paddedHeight = ((height + 63) / 64) * 64;
    const auto plane = static_cast<std::size_t>(paddedWidth) * static_cast<std::size_t>(paddedHeight);
    media::RifeSdrFrame result{width,height,paddedWidth,paddedHeight,{},std::vector<float>(plane * 3,0.5F)};
    std::mt19937 random(87943);
    std::normal_distribution<float> normal(0.0F,static_cast<float>(sigma255 / 255.0));
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto offset = channel * plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(paddedWidth) + static_cast<std::size_t>(x);
            result.pixels[offset] = std::clamp(0.5F + normal(random),0.0F,1.0F);
        }
    return result;
}
}

TEST(GaussianNoiseAnalysis, RecoversKnownGaussianConditioningWithinOneLevel) {
    for (const double sigma : {5.0,15.0,25.0}) {
        const auto estimate = estimateGaussianNoise(noiseFrame(384,256,sigma));
        EXPECT_NEAR(estimate.sigma255,sigma,1.0);
        EXPECT_GT(estimate.confidence,0.9);
        EXPECT_LE(estimate.samplesPerChannel,16384U);
    }
}

TEST(GaussianNoiseAnalysis, CleanLinearContentIsNotMistakenForNoiseAndPaddingIsExcluded) {
    auto image = noiseFrame(191,129);
    const auto plane = static_cast<std::size_t>(image.paddedWidth) * static_cast<std::size_t>(image.paddedHeight);
    std::fill(image.pixels.begin(),image.pixels.end(),std::numeric_limits<float>::quiet_NaN());
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x)
            image.pixels[channel * plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.paddedWidth) + static_cast<std::size_t>(x)] =
                static_cast<float>(0.2 + x * 0.001 + y * 0.002);
    const auto estimate = estimateGaussianNoise(image);
    EXPECT_DOUBLE_EQ(estimate.sigma255,0.0);
    EXPECT_GT(estimate.confidence,0.9);
}

TEST(GaussianNoiseAnalysis, StructuredPatternsClippingAndInsufficientSamplesStayUntrusted) {
    auto checker = noiseFrame(192,192);
    const auto plane = static_cast<std::size_t>(checker.paddedWidth) * static_cast<std::size_t>(checker.paddedHeight);
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (int y = 0; y < checker.height; ++y) for (int x = 0; x < checker.width; ++x)
            checker.pixels[channel * plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(checker.paddedWidth) + static_cast<std::size_t>(x)] =
                (x + y) % 2 ? 0.6F : 0.4F;
    EXPECT_DOUBLE_EQ(estimateGaussianNoise(checker).confidence,0.0);
    std::fill(checker.pixels.begin(),checker.pixels.end(),0.0F);
    EXPECT_DOUBLE_EQ(estimateGaussianNoise(checker).confidence,0.0);
    EXPECT_DOUBLE_EQ(estimateGaussianNoise(noiseFrame(16,16,20)).confidence,0.0);
}

TEST(GaussianNoiseAnalysis, SamplingStorageIsBoundedAndCancellationAndInvalidTensorsFail) {
    const auto estimate = estimateGaussianNoise(noiseFrame(1024,768,15));
    EXPECT_LE(estimate.samplesPerChannel,16384U);
    EXPECT_NEAR(estimate.sigma255,15.0,1.0);
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW((void)estimateGaussianNoise(noiseFrame(64,64),stop.get_token()),std::runtime_error);
    auto image = noiseFrame(64,64);
    image.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW((void)estimateGaussianNoise(image),std::invalid_argument);
    image = noiseFrame(64,64); image.pixels.pop_back();
    EXPECT_THROW((void)estimateGaussianNoise(image),std::invalid_argument);
}

TEST(AutomaticGaussianDenoise, SmoothsInSourceTimeResetsAtCutsAndBypassesAmbiguousFrames) {
    AutomaticDenoiseSettings settings;
    settings.maximumSigmaChangePerSecond = 15.0;
    AutomaticDenoiseController controller(settings);
    EXPECT_FLOAT_EQ(controller.update(0,{10.0,1.0,1000}).sigma255,10.0F);
    const auto next = controller.update(Rational(1,30),{40.0,1.0,1000});
    EXPECT_GT(next.sigma255,10.0F);
    EXPECT_LE(next.sigma255,10.500001F);
    const auto uncertain = controller.update(Rational(2,30),{40.0,0.2,1000});
    EXPECT_FALSE(uncertain.confident);
    EXPECT_FLOAT_EQ(uncertain.sigma255,0.0F);
    const auto cut = controller.update(Rational(3,30),{30.0,1.0,1000},true);
    EXPECT_FLOAT_EQ(cut.sigma255,30.0F);
    const auto clean = controller.update(Rational(4,30),{0.0,1.0,1000},true);
    EXPECT_TRUE(clean.confident);
    EXPECT_FLOAT_EQ(clean.sigma255,0.0F);
    EXPECT_THROW((void)controller.update(Rational(4,30),{10.0,1.0,1000}),std::invalid_argument);
}

TEST(AutomaticGaussianDenoise, RelativeControlsAreLimitedToTheActualModelDomain) {
    AutomaticDenoiseSettings settings; settings.relativeAdjustment = -1.0;
    AutomaticDenoiseController disabled(settings);
    EXPECT_FLOAT_EQ(disabled.update(0,{25.0,1.0,1000}).sigma255,0.0F);
    settings.relativeAdjustment = 1.0;
    AutomaticDenoiseController strongest(settings);
    EXPECT_FLOAT_EQ(strongest.update(0,{25.0,1.0,1000}).sigma255,55.0F);
    EXPECT_FLOAT_EQ(strongest.update(1,{150.0,1.0,1000}).sigma255,0.0F);
    settings.minimumConfidence = 0.0;
    AutomaticDenoiseController noFakeEstimate(settings);
    const auto missing = noFakeEstimate.update(0,{});
    EXPECT_FALSE(missing.confident);
    EXPECT_FLOAT_EQ(missing.sigma255,0.0F);
    settings.relativeAdjustment = 2;
    EXPECT_THROW((void)AutomaticDenoiseController(settings),std::invalid_argument);
    settings.relativeAdjustment = 0; settings.minimumConfidence = -0.1;
    EXPECT_THROW((void)AutomaticDenoiseController(settings),std::invalid_argument);
}

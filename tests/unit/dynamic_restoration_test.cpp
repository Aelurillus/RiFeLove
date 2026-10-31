#include "restoration/dynamic_control.hpp"
#include "restoration/temporal_window.hpp"
#include "restoration/noise_estimate.hpp"
#include "restoration/compression_estimate.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
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

TEST(DynamicRestoration, ZeroConfidenceNeverChangesStateAtZeroThreshold) {
    DynamicSettings settings; settings.minimumConfidence = 0;
    DynamicController controller(settings,"zero-threshold");
    const auto absent = controller.update(observation(0,.9,0));
    EXPECT_FALSE(absent.hasEstimate[0]);
    EXPECT_DOUBLE_EQ(absent.strength[0],0);
    const auto trusted = controller.update(observation(1,.8,1));
    ASSERT_DOUBLE_EQ(trusted.automatic[0],.8);
    const auto ambiguous = controller.update(observation(11,0,0));
    EXPECT_DOUBLE_EQ(ambiguous.automatic[0],.8);
    const auto trustedAgain = controller.update(observation(Rational(331,30),.8,1));
    EXPECT_DOUBLE_EQ(trustedAgain.automatic[0],.8);
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

TEST(TemporalRestorationWindow, DeclaresSceneBoundsForEdgePaddingAtCutAndEof) {
    TemporalWindow window(temporalCaps());
    std::vector<Request> requests;
    for (std::size_t index = 0; index < 4; ++index) {
        auto ready = window.push(frame(index));
        requests.insert(requests.end(),ready.begin(),ready.end());
    }
    auto cut = window.push(frame(4),true);
    requests.insert(requests.end(),cut.begin(),cut.end());
    auto end = window.finish();
    requests.insert(requests.end(),end.begin(),end.end());
    ASSERT_EQ(requests.size(),5U);
    for (const auto& request : requests) {
        ASSERT_TRUE(request.sceneBounds);
        EXPECT_GT(request.sceneBounds->serial,0U);
        const auto center = request.context[request.centerIndex].index;
        if (center < 4) {
            EXPECT_EQ(request.sceneBounds->firstIndex,0U);
            if (request.missingFramesAfter) {
                ASSERT_TRUE(request.sceneBounds->lastIndex);
                EXPECT_EQ(*request.sceneBounds->lastIndex,3U);
            }
        } else {
            EXPECT_EQ(request.sceneBounds->firstIndex,4U);
            ASSERT_TRUE(request.sceneBounds->lastIndex);
            EXPECT_EQ(*request.sceneBounds->lastIndex,4U);
        }
    }
    EXPECT_EQ(requests.back().sceneBounds->serial,2U);
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
    if (sigma255 <= 0.0) return result;
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

TEST(GaussianNoiseAnalysis, SparsePeriodicTextureCannotAlignWithEverySample) {
    // One in six 3x3 blocks contains a random texture; the rest is clean.
    // At this resolution the former fixed stride sampled only those blocks,
    // producing a spurious high-confidence full-frame noise estimate.
    for (int phase = 0; phase < 6; ++phase) {
        auto image = noiseFrame(1024,768);
        const auto plane = static_cast<std::size_t>(image.paddedWidth) * image.paddedHeight;
        const int columns = image.width / 3, rows = image.height / 3;
        std::mt19937 random(6271);
        std::normal_distribution<float> normal(0.0F,20.0F / 255.0F);
        for (int row = 0; row < rows; ++row) for (int column = 0; column < columns; ++column) {
            if ((row * columns + column) % 6 != phase) continue;
            for (std::size_t channel = 0; channel < 3; ++channel)
                for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 3; ++dx) {
                    const auto offset = channel * plane + static_cast<std::size_t>(row * 3 + dy) * image.paddedWidth + column * 3 + dx;
                    image.pixels[offset] = std::clamp(0.5F + normal(random),0.0F,1.0F);
                }
        }
        if (phase == 0) {
            const auto legacy = estimateGaussianNoise(image,{},1);
            EXPECT_GT(legacy.sigma255,15.0);
            EXPECT_GT(legacy.confidence,0.9);
        }
        const auto result = estimateGaussianNoise(image,{},2);
        EXPECT_LT(result.sigma255,5.0) << "phase=" << phase;
        const auto replay = estimateGaussianNoise(image,{},2);
        EXPECT_DOUBLE_EQ(result.sigma255,replay.sigma255);
        EXPECT_DOUBLE_EQ(result.confidence,replay.confidence);
        EXPECT_EQ(result.samplesPerChannel,replay.samplesPerChannel);
        EXPECT_THROW((void)estimateGaussianNoise(image,{},3),std::invalid_argument);
    }
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

TEST(AutomaticGaussianDenoise, MissingEvidenceDoesNotPoisonZeroThresholdSmoothing) {
    AutomaticDenoiseSettings settings; settings.minimumConfidence = 0;
    AutomaticDenoiseController controller(settings);
    EXPECT_FLOAT_EQ(controller.update(0,{25.0,1.0,1000}).sigma255,25.0F);
    const auto absent = controller.update(10,{0.0,0.0,0});
    EXPECT_FALSE(absent.confident);
    EXPECT_FLOAT_EQ(absent.sigma255,0.0F);
    const auto next = controller.update(Rational(301,30),{25.0,1.0,1000});
    EXPECT_TRUE(next.confident);
    EXPECT_FLOAT_EQ(next.sigma255,25.0F);
}

namespace {
template<class Pixel> media::DecodedFrame compressionFrame(int width, int height, Pixel pixel) {
    media::DecodedFrame image{};
    image.width = width; image.height = height; image.timestamp = Rational(0);
    image.color.pixelFormat = "yuv420p"; image.color.bitDepth = 8;
    image.color.primaries = "bt709"; image.color.transfer = "bt709";
    image.color.matrix = "bt709"; image.color.range = "tv";
    const int stride = width + 13;
    auto allocation = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(stride) * static_cast<std::size_t>(height) + 29,255);
    auto* data = allocation->data() + 29;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        data[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(pixel(x,y));
    image.planes[0] = {data,stride,media::FramePlaneExtent{allocation->data(),allocation->size(),29}};
    image.storage = allocation;
    return image;
}
}

TEST(CompressionArtifactAnalysis, DetectsDistributedBlockEvidenceWithoutReadingStridePadding) {
    const auto image = compressionFrame(256,192,[](int x,int y) { return 100 + ((x / 8 + y / 8) % 2) * 16; });
    const auto estimate = estimateCompressionArtifacts(image);
    EXPECT_GT(estimate.strength,0.8);
    EXPECT_GT(estimate.confidence,0.9);
    EXPECT_LE(estimate.samples,16384U);
    const auto clean = estimateCompressionArtifacts(compressionFrame(256,192,[](int x,int y) { return 60 + x / 4 + y / 4; }));
    EXPECT_DOUBLE_EQ(clean.strength,0.0);
    EXPECT_GT(clean.confidence,0.9);
}

TEST(CompressionArtifactAnalysis, IsolatedEdgesBusyTextureClippingAndSmallSamplesAreNotTrusted) {
    const auto edge = compressionFrame(256,192,[](int x,int) { return x < 128 ? 100 : 116; });
    EXPECT_LT(estimateCompressionArtifacts(edge).confidence,0.6);
    const auto texture = compressionFrame(256,192,[](int x,int y) { return (x + y) % 2 ? 88 : 136; });
    EXPECT_DOUBLE_EQ(estimateCompressionArtifacts(texture).confidence,0.0);
    const auto clipped = compressionFrame(256,192,[](int,int) { return 16; });
    EXPECT_DOUBLE_EQ(estimateCompressionArtifacts(clipped).confidence,0.0);
    const auto small = compressionFrame(16,16,[](int,int) { return 100; });
    EXPECT_DOUBLE_EQ(estimateCompressionArtifacts(small).confidence,0.0);
}

TEST(CompressionArtifactAnalysis, SubsampledFullHdCoversBlockPhasesInBothOrientations) {
    for (const auto dimensions : {std::array{1920,1080},std::array{1080,1920}}) {
        for (const int block : {8,16,32}) for (const int phase : {0,8}) {
            SCOPED_TRACE(::testing::Message() << dimensions[0] << 'x' << dimensions[1]
                << " block=" << block << " phase=" << phase);
            const auto image = compressionFrame(dimensions[0],dimensions[1],[=](int x,int y) {
                return 100 + (((x + phase) / block + (y + phase) / block) % 2) * 16;
            });
            const auto estimate = estimateCompressionArtifacts(image);
            EXPECT_GT(estimate.strength,0.25);
            EXPECT_GT(estimate.confidence,0.9);
            EXPECT_LE(estimate.samples,16384U);
            const auto replayed = estimateCompressionArtifacts(image);
            EXPECT_DOUBLE_EQ(replayed.strength,estimate.strength);
            EXPECT_DOUBLE_EQ(replayed.confidence,estimate.confidence);
            EXPECT_EQ(replayed.samples,estimate.samples);
        }
    }
}

TEST(CompressionArtifactAnalysis, SubsampledFullHdKeepsCleanEdgesAndTextureConservative) {
    for (const auto dimensions : {std::array{1920,1080},std::array{1080,1920}}) {
        SCOPED_TRACE(::testing::Message() << dimensions[0] << 'x' << dimensions[1]);
        const auto clean = estimateCompressionArtifacts(compressionFrame(dimensions[0],dimensions[1],[](int x,int y) {
            return 80 + x / 32 + y / 32;
        }));
        EXPECT_DOUBLE_EQ(clean.strength,0.0);
        EXPECT_GT(clean.confidence,0.9);
        EXPECT_LE(clean.samples,16384U);
        for (const bool vertical : {false,true}) {
            SCOPED_TRACE(::testing::Message() << "vertical=" << vertical);
            const int edge = (dimensions[vertical ? 0 : 1] / 16) * 8;
            const auto isolated = estimateCompressionArtifacts(compressionFrame(dimensions[0],dimensions[1],[=](int x,int y) {
                return (vertical ? x : y) < edge ? 100 : 116;
            }));
            EXPECT_DOUBLE_EQ(isolated.confidence,0.0);
            const auto stripes = estimateCompressionArtifacts(compressionFrame(dimensions[0],dimensions[1],[=](int x,int y) {
                return 100 + (((vertical ? x : y) / 16) % 2) * 16;
            }));
            EXPECT_DOUBLE_EQ(stripes.confidence,0.0);
        }
        const auto texture = estimateCompressionArtifacts(compressionFrame(dimensions[0],dimensions[1],[](int x,int y) {
            return (x + y) % 2 ? 88 : 136;
        }));
        EXPECT_DOUBLE_EQ(texture.confidence,0.0);
    }
}

TEST(CompressionArtifactAnalysis, RequiresProvenUnmodifiedSdrLumaAndHonorsCancellation) {
    auto image = compressionFrame(128,128,[](int,int) { return 100; });
    image.planes[0].extent.reset();
    EXPECT_THROW((void)estimateCompressionArtifacts(image),std::invalid_argument);
    image = compressionFrame(128,128,[](int,int) { return 100; });
    --image.planes[0].extent->dataOffset;
    EXPECT_THROW((void)estimateCompressionArtifacts(image),std::invalid_argument);
    image = compressionFrame(128,128,[](int,int) { return 100; });
    image.planes[0].extent->allocationBytes = 30;
    EXPECT_THROW((void)estimateCompressionArtifacts(image),std::invalid_argument);
    image = compressionFrame(128,128,[](int,int) { return 100; });
    image.color.transfer = "smpte2084";
    EXPECT_THROW((void)estimateCompressionArtifacts(image),std::invalid_argument);
    image = compressionFrame(128,128,[](int,int) { return 100; });
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW((void)estimateCompressionArtifacts(image,stop.get_token()),std::runtime_error);
    const auto large = estimateCompressionArtifacts(compressionFrame(1024,768,[](int,int) { return 100; }));
    EXPECT_LE(large.samples,16384U);
}

TEST(AutomaticCompressionRestoration, UsesExactSourceTimeResetsAtCutsAndBypassesAmbiguousFrames) {
    AutomaticCompressionSettings settings;
    settings.maximumStrengthChangePerSecond = 0.3;
    AutomaticCompressionController control(settings);
    EXPECT_FLOAT_EQ(control.update(0,{0.2,1.0,1000}).strength,0.2F);
    const auto next = control.update(Rational(1,30),{0.9,1.0,1000});
    EXPECT_GT(next.strength,0.2F);
    EXPECT_LE(next.strength,0.210001F);
    const auto ambiguous = control.update(Rational(2,30),{0.9,0.1,1000});
    EXPECT_FALSE(ambiguous.confident); EXPECT_FLOAT_EQ(ambiguous.strength,0.0F);
    EXPECT_FLOAT_EQ(control.update(Rational(3,30),{0.8,1.0,1000},true).strength,0.8F);
    EXPECT_FLOAT_EQ(control.update(Rational(4,30),{0.0,1.0,1000},true).strength,0.0F);
    EXPECT_THROW((void)control.update(Rational(4,30),{0.3,1.0,1000}),std::invalid_argument);
}

TEST(AutomaticCompressionRestoration, FullHdSourceEvidenceResetsAtCutsAndBypassesTexture) {
    const auto blocked = estimateCompressionArtifacts(compressionFrame(1920,1080,[](int x,int y) {
        return 100 + ((x / 16 + y / 16) % 2) * 16;
    }));
    const auto clean = estimateCompressionArtifacts(compressionFrame(1920,1080,[](int,int) { return 100; }));
    const auto texture = estimateCompressionArtifacts(compressionFrame(1920,1080,[](int x,int y) {
        return (x + y) % 2 ? 88 : 136;
    }));
    ASSERT_GT(blocked.confidence,0.9);
    ASSERT_GT(blocked.strength,0.5);
    ASSERT_GT(clean.confidence,0.9);
    ASSERT_DOUBLE_EQ(texture.confidence,0.0);
    AutomaticCompressionController control(AutomaticCompressionSettings{});
    const auto initial = control.update(0,blocked);
    EXPECT_TRUE(initial.confident);
    EXPECT_FLOAT_EQ(initial.strength,static_cast<float>(blocked.strength));
    EXPECT_GT(control.update(Rational(1,30),clean).strength,0.0F);
    const auto cleanCut = control.update(Rational(2,30),clean,true);
    EXPECT_TRUE(cleanCut.confident);
    EXPECT_FLOAT_EQ(cleanCut.strength,0.0F);
    const auto uncertain = control.update(Rational(3,30),texture);
    EXPECT_FALSE(uncertain.confident);
    EXPECT_FLOAT_EQ(uncertain.strength,0.0F);
    const auto blockCut = control.update(Rational(4,30),blocked,true);
    EXPECT_TRUE(blockCut.confident);
    EXPECT_FLOAT_EQ(blockCut.strength,static_cast<float>(blocked.strength));
}

TEST(AutomaticCompressionRestoration, RelativeControlsCannotManufactureMissingEvidence) {
    AutomaticCompressionSettings settings; settings.relativeAdjustment = -1;
    AutomaticCompressionController disabled(settings);
    EXPECT_FLOAT_EQ(disabled.update(0,{0.5,1.0,1000}).strength,0.0F);
    settings.relativeAdjustment = 1;
    AutomaticCompressionController strongest(settings);
    EXPECT_FLOAT_EQ(strongest.update(0,{0.5,1.0,1000}).strength,1.0F);
    EXPECT_FLOAT_EQ(strongest.update(1,{}).strength,0.0F);
    settings.minimumConfidence = 0;
    AutomaticCompressionController zeroThreshold(settings);
    EXPECT_FALSE(zeroThreshold.update(0,{}).confident);
    settings.relativeAdjustment = 1.1;
    EXPECT_THROW((void)AutomaticCompressionController(settings),std::invalid_argument);
    settings.relativeAdjustment = 0; settings.smoothingSeconds = 0;
    EXPECT_THROW((void)AutomaticCompressionController(settings),std::invalid_argument);
}

TEST(AutomaticCompressionRestoration, MissingEvidenceDoesNotPoisonZeroThresholdSmoothing) {
    AutomaticCompressionSettings settings; settings.minimumConfidence = 0;
    AutomaticCompressionController controller(settings);
    EXPECT_FLOAT_EQ(controller.update(0,{.8,1.0,1000}).strength,.8F);
    const auto absent = controller.update(10,{0.0,0.0,0});
    EXPECT_FALSE(absent.confident);
    EXPECT_FLOAT_EQ(absent.strength,0.0F);
    const auto next = controller.update(Rational(301,30),{.8,1.0,1000});
    EXPECT_TRUE(next.confident);
    EXPECT_FLOAT_EQ(next.strength,.8F);
}

TEST(AutomaticCompressionRestoration, FullSourceReplayReproducesVfrSceneAndAmbiguityHistory) {
    std::vector<AutomaticCompressionValue> expected;
    AutomaticCompressionController uninterrupted(AutomaticCompressionSettings{});
    for (int i = 0; i < 60; ++i)
        expected.push_back(uninterrupted.update(Rational(i * i + i,1000),{i % 2 ? 0.2 : 0.8,i % 7 ? 1.0 : 0.0,1000},i == 27));
    AutomaticCompressionController replayed(AutomaticCompressionSettings{});
    for (int i = 0; i < 60; ++i) {
        const auto value = replayed.update(Rational(i * i + i,1000),{i % 2 ? 0.2 : 0.8,i % 7 ? 1.0 : 0.0,1000},i == 27);
        EXPECT_FLOAT_EQ(value.strength,expected[static_cast<std::size_t>(i)].strength);
        EXPECT_EQ(value.confident,expected[static_cast<std::size_t>(i)].confident);
    }
}

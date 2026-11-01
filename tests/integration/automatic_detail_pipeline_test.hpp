#pragma once

#include "motion_blur_pipeline_test.hpp"
#include "application/job_serialization.hpp"

#include <algorithm>
#include <array>

namespace {
class RecordingDetailBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    explicit RecordingDetailBackend(std::vector<std::string>* order = nullptr) : order_(order) {}
    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        return {"test-detail","test-deblur-v1",1,{Task::deblur},
            {Placement::outputRateAfterInterpolation},{"bt709-sdr8"},0,0,1,1,false};
    }
    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
        if (order_) order_->push_back("deblur");
        const auto& center = request.context[request.centerIndex];
        const float strength = request.deblurStrength.value_or(-1.0F);
        strengths.push_back(strength);
        timestamps.push_back(*center.timestamp);
        auto output = center;
        if (strength > 0.0F) {
            auto pixels = rifelove::media::prepareRifeSdr(center);
            for (auto& value : pixels.pixels) value = std::clamp(value + strength / 10.0F,0.0F,1.0F);
            output = rifelove::media::restoreRifeSdr(pixels);
            output.index = center.index; output.timestamp = center.timestamp; output.duration = center.duration;
            output.timestampRecovered = center.timestampRecovered;
        }
        std::promise<rifelove::restoration::Result> promise;
        promise.set_value({std::move(output),0.0});
        return promise.get_future();
    }
    std::vector<float> strengths;
    std::vector<Rational> timestamps;
private:
    std::vector<std::string>* order_ = nullptr;
};

class RecordingSuperResolutionBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    explicit RecordingSuperResolutionBackend(std::vector<std::string>& order) : order_(order) {}
    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        return {"test-sr","test-sr-v1",1,{Task::superResolution},
            {Placement::outputRateAfterInterpolation},{"bt709-sdr8"},0,0,2,1,false};
    }
    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
        order_.push_back("sr");
        const auto& center = request.context.at(request.centerIndex);
        auto input = rifelove::media::prepareRifeSdr(center);
        const int width = input.width * 2, height = input.height * 2;
        const int paddedWidth = ((width + 63) / 64) * 64, paddedHeight = ((height + 63) / 64) * 64;
        const auto sourcePlane = static_cast<std::size_t>(input.paddedWidth) * input.paddedHeight;
        const auto outputPlane = static_cast<std::size_t>(paddedWidth) * paddedHeight;
        rifelove::media::RifeSdrFrame enlarged{width,height,paddedWidth,paddedHeight,input.sourceColor,
            std::vector<float>(outputPlane * 3)};
        for (int y = 0; y < paddedHeight; ++y) for (int x = 0; x < paddedWidth; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel)
                enlarged.pixels[channel * outputPlane + static_cast<std::size_t>(y) * paddedWidth + x] =
                    input.pixels[channel * sourcePlane + static_cast<std::size_t>(std::min(y / 2,input.height - 1)) * input.paddedWidth +
                        std::min(x / 2,input.width - 1)];
        auto output = rifelove::media::restoreRifeSdr(enlarged);
        output.index = center.index; output.timestamp = center.timestamp; output.duration = center.duration;
        output.timestampRecovered = center.timestampRecovered;
        rifelove::restoration::Result result{std::move(output),0.0};
        rifelove::restoration::validateResult(capabilities(),request,result);
        std::promise<rifelove::restoration::Result> promise;
        promise.set_value(std::move(result));
        return promise.get_future();
    }
private:
    std::vector<std::string>& order_;
};

class AutomaticDetailPipeline : public MotionBlurPipeline {
protected:
    void verifyIndependentDeblurThenSuperResolution() {
        const auto source = directory / "dual-image-source.mkv";
        writeExposureSource(source,30,3);
        ExposureLinearBackend inference(*device);
        std::vector<std::string> order;
        auto deblur = std::make_shared<RecordingDetailBackend>(&order);
        auto sr = std::make_shared<RecordingSuperResolutionBackend>(order);
        auto config = settings();
        config.motionBlur.enabled = false;
        config.automaticDetailRestoration.emplace();
        config.deblurRestorationBackend = deblur;
        config.restorationBackend = sr;
        const auto output = directory / "dual-image-output.mkv";
        const auto result = rifelove::interpolateVideo(source.string(),output.string(),config,*device,inference);
        EXPECT_EQ(result.deblurRestorationModelId,"test-detail");
        EXPECT_EQ(result.restorationModelId,"test-sr");
        EXPECT_EQ(result.validation.width,128);
        EXPECT_EQ(result.validation.height,128);
        ASSERT_EQ(order.size(),result.video.framesSubmitted * 2);
        ASSERT_EQ(deblur->strengths.size(),result.video.framesSubmitted);
        for (std::size_t index = 0; index < order.size(); index += 2) {
            EXPECT_EQ(order[index],"deblur");
            EXPECT_EQ(order[index + 1],"sr");
        }

        config.restorationBackend.reset();
        config.automaticDetailRestoration.reset();
        const auto deblurOnly = rifelove::interpolateVideo(source.string(),
            (directory / "deblur-only.mkv").string(),config,*device,inference);
        EXPECT_EQ(deblurOnly.validation.width,64);
        EXPECT_EQ(deblurOnly.validation.height,64);
        EXPECT_EQ(deblurOnly.deblurRestorationModelId,"test-detail");

        config.deblurRestorationBackend.reset();
        config.restorationBackend = sr;
        const auto srOnly = rifelove::interpolateVideo(source.string(),
            (directory / "sr-only.mkv").string(),config,*device,inference);
        EXPECT_EQ(srOnly.validation.width,128);
        EXPECT_EQ(srOnly.validation.height,128);
        EXPECT_TRUE(srOnly.deblurRestorationModelId.empty());

        config.deblurRestorationBackend = sr;
        EXPECT_THROW((void)rifelove::interpolateVideo(source.string(),
            (directory / "wrong-deblur-slot.mkv").string(),config,*device,inference),std::invalid_argument);
    }
    void verifySourcePtsSceneBypassAndCheckpointReplay() {
        const auto source = directory / "detail-source.mkv";
        writeExposureSource(source,30,8,4,[](int index,int x,int y,int) {
            if (index >= 4) return 220;
            constexpr std::array<int,7> weights{1,2,3,4,3,2,1};
            int value = 0;
            for (int offset = -3; offset <= 3; ++offset) {
                const int xx = std::clamp(x + index + offset,0,383);
                value += (((xx / 32 + y / 32) & 1) ? 192 : 64) * weights[static_cast<std::size_t>(offset + 3)];
            }
            return value / 16;
        },AV_PIX_FMT_YUV444P,384,256);
        const auto sourcePixels = readExposurePixels(source);
        ASSERT_EQ(sourcePixels.timestamps.size(),8U);
        ExposureLinearBackend inference(*device);
        auto config = settings();
        config.motionBlur.enabled = false;
        config.automaticDetailRestoration.emplace();
        config.automaticDetailRestoration->minimumConfidence = 0.2;
        config.automaticDetailRestoration->maximumStrengthChangePerSecond = 10.0;
        const auto checkpoint = [&](const std::string& prefix,std::vector<rifelove::media::VideoSegmentInput>& segments) {
            rifelove::PipelineCheckpoint value;
            value.framesPerSegment = 4;
            value.restorationIdentityVerified = true; // Manifest tests bind the settings and model.
            value.completedSegments = segments;
            value.segmentPath = [&,prefix](std::uint64_t index) {
                return (directory / (prefix + "-segment-" + std::to_string(index) + ".mkv")).string();
            };
            value.commitSegment = [&segments](std::uint64_t index,std::uint64_t,std::uint64_t,
                const rifelove::media::VideoSegmentInput& segment) {
                EXPECT_EQ(index,segments.size()); segments.push_back(segment);
            };
            return value;
        };
        std::vector<rifelove::media::VideoSegmentInput> referenceSegments;
        auto referenceDetail = std::make_shared<RecordingDetailBackend>();
        config.restorationBackend = referenceDetail;
        config.checkpoint = checkpoint("detail-reference",referenceSegments);
        const auto referencePath = directory / "detail-reference.mkv";
        const auto reference = rifelove::interpolateVideo(source.string(),referencePath.string(),config,*device,inference);
        const auto referencePixels = readExposurePixels(referencePath);
        EXPECT_EQ(reference.sourceDecodeAcceleration,"software");
        EXPECT_EQ(reference.automaticDetailAnalyzedFrames,8U);
        EXPECT_GT(reference.automaticDetailConfidentFrames,0U);
        EXPECT_GT(reference.automaticDetailAppliedFrames,0U);
        ASSERT_EQ(referenceDetail->strengths.size(),referencePixels.timestamps.size());
        for (std::size_t i = 0; i < referenceDetail->strengths.size(); ++i) {
            // The backend sees exact 60 FPS PTS; Matroska rounds decoded PTS to milliseconds.
            EXPECT_EQ(referenceDetail->timestamps[i],Rational(static_cast<std::int64_t>(i),60));
            EXPECT_LE(std::abs((referenceDetail->timestamps[i] - referencePixels.timestamps[i]).toLongDouble()),0.001L);
            EXPECT_GE(referenceDetail->strengths[i],0.0F);
            EXPECT_LE(referenceDetail->strengths[i],rifelove::restoration::maximumAutomaticDeblurBlend);
            if (referenceDetail->timestamps[i] >= sourcePixels.timestamps[4])
                EXPECT_EQ(referenceDetail->strengths[i],0.0F);
        }
        std::vector<rifelove::media::VideoSegmentInput> interruptedSegments;
        config.restorationBackend = std::make_shared<RecordingDetailBackend>();
        config.checkpoint = checkpoint("detail-resume",interruptedSegments);
        const auto resumedPath = directory / "detail-resume.mkv";
        std::stop_source stop;
        EXPECT_ANY_THROW((void)rifelove::interpolateVideo(source.string(),resumedPath.string(),config,*device,inference,
            [&](const rifelove::PipelineProgress& event) {
                if (event.outputFrames >= 6) stop.request_stop();
            },stop.get_token()));
        ASSERT_TRUE(stop.stop_requested());
        ASSERT_FALSE(interruptedSegments.empty());
        auto resumedDetail = std::make_shared<RecordingDetailBackend>();
        config.restorationBackend = resumedDetail;
        config.checkpoint = checkpoint("detail-resume",interruptedSegments);
        const auto resumed = rifelove::interpolateVideo(source.string(),resumedPath.string(),config,*device,inference);
        const auto resumedPixels = readExposurePixels(resumedPath);
        EXPECT_EQ(resumed.automaticDetailAnalyzedFrames,8U);
        EXPECT_GT(resumed.reusedFrames,0U);
        EXPECT_EQ(resumed.processedFrames + resumed.reusedFrames,reference.video.framesSubmitted);
        EXPECT_EQ(resumedPixels.timestamps,referencePixels.timestamps);
        EXPECT_EQ(resumedPixels.frames,referencePixels.frames);
        EXPECT_EQ(resumedDetail->timestamps.size(),resumed.processedFrames);
    }
};
} // namespace

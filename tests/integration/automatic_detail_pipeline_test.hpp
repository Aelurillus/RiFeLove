#pragma once

#include "motion_blur_pipeline_test.hpp"
#include "application/job_serialization.hpp"

#include <algorithm>
#include <array>

namespace {
class RecordingDetailBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        return {"test-detail","test-deblur-v1",1,{Task::deblur},
            {Placement::outputRateAfterInterpolation},{"bt709-sdr8"},0,0,1,1,false};
    }
    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
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
};

class AutomaticDetailPipeline : public MotionBlurPipeline {
protected:
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

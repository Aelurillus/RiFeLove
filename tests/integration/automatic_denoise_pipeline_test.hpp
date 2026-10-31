#pragma once

#include "motion_blur_pipeline_test.hpp"
#include "application/job_serialization.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {
// The identity backend records real production requests. These tests establish
// conditioning/scene/timeline plumbing, not neural denoising quality.
class RecordingTemporalDenoiseBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        return {"test-temporal","fastdvdnet-rgb-f32-nchw16-v1",1,{Task::denoise},
            {Placement::sourceRateBeforeInterpolation},{"bt709-sdr8"},2,2,1,1,true};
    }
    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
        if (!request.denoiseSigma255) throw std::runtime_error("Missing real denoise conditioning");
        sigmas.push_back(*request.denoiseSigma255);
        const auto& center = request.context[request.centerIndex];
        timestamps.push_back(*center.timestamp);
        if (order) order->push_back("temporal:" + std::to_string(center.index));
        for (const auto& frame : request.context)
            EXPECT_EQ(frame.index < 6,center.index < 6) << "Temporal input crossed the fixture scene cut";
        auto output = center;
        if (conditioningChangesPixels && *request.denoiseSigma255 > 0.0F) {
            auto pixels = rifelove::media::prepareRifeSdr(center);
            for (auto& value : pixels.pixels) value = std::clamp(value + *request.denoiseSigma255 / 5500.0F,0.0F,1.0F);
            output = rifelove::media::restoreRifeSdr(pixels);
            output.index = center.index; output.timestamp = center.timestamp; output.duration = center.duration;
            output.timestampRecovered = center.timestampRecovered;
        }
        std::promise<rifelove::restoration::Result> promise;
        promise.set_value({std::move(output),0.0});
        return promise.get_future();
    }
    std::vector<float> sigmas;
    std::vector<Rational> timestamps;
    std::vector<std::string>* order = nullptr;
    bool conditioningChangesPixels = false; // Protocol fixture, not neural enhancement.
};

class RecordingCompressionBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    struct RequestRecord {
        std::size_t centerIndex = 0;
        std::size_t contextSize = 0;
        std::size_t missingBefore = 0;
        std::size_t missingAfter = 0;
        std::size_t sourceIndex = 0;
        rifelove::Rational timestamp;
        std::uint64_t sceneSerial = 0;
        std::size_t sceneFirstIndex = 0;
    };
    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        return {"test-deblock","stdf-test-v1",1,tasks,
            {Placement::sourceRateBeforeInterpolation},{"bt709-sdr8"},framesBefore,framesAfter,1,1,true};
    }
    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
        if (!request.sceneBounds) throw std::runtime_error("Compression fixture requires scene bounds");
        const auto& center = request.context[request.centerIndex];
        requests.push_back({request.centerIndex,request.context.size(),request.missingFramesBefore,
            request.missingFramesAfter,center.index,*center.timestamp,request.sceneBounds->serial,request.sceneBounds->firstIndex});
        if (order) order->push_back("compression:" + std::to_string(center.index));
        std::promise<rifelove::restoration::Result> promise;
        promise.set_value({center,0.0});
        return promise.get_future();
    }
    std::vector<RequestRecord> requests;
    std::vector<std::string>* order = nullptr;
    std::vector<rifelove::restoration::Task> tasks{rifelove::restoration::Task::deblock};
    std::size_t framesBefore = 3;
    std::size_t framesAfter = 3;
};

class AutomaticDenoisePipeline : public MotionBlurPipeline {
protected:
    void verifyCompressionShortClip() {
        const auto source = directory / "short-source.mkv";
        writeExposureSource(source,30,1);
        ExposureLinearBackend backend(*device);
        auto compression = std::make_shared<RecordingCompressionBackend>();
        auto config = settings();
        config.motionBlur.enabled = false;
        config.compressionRestorationBackend = compression;
        const auto output = directory / "short-compression.mkv";
        const auto result = rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend);
        ASSERT_EQ(compression->requests.size(),1U);
        EXPECT_EQ(compression->requests.front().contextSize,1U);
        EXPECT_EQ(compression->requests.front().missingBefore,3U);
        EXPECT_EQ(compression->requests.front().missingAfter,3U);
        EXPECT_EQ(compression->requests.front().sceneSerial,1U);
        EXPECT_EQ(compression->requests.front().sceneFirstIndex,0U);
        EXPECT_EQ(result.sourceDecodeAcceleration,"software");
        EXPECT_EQ(result.validation.decodedFrames,2U);
    }

    void verifyCompressionStage() {
        const auto source = directory / "source.mkv";
        writeExposureSource(source,30,12,6);
        ExposureLinearBackend backend(*device);
        auto compression = std::make_shared<RecordingCompressionBackend>();
        auto temporal = std::make_shared<RecordingTemporalDenoiseBackend>();
        std::vector<std::string> order;
        compression->order = &order;
        temporal->order = &order;
        auto config = settings();
        config.motionBlur.enabled = false;
        config.compressionRestorationBackend = compression;
        config.temporalRestorationBackend = temporal;
        const auto output = directory / "compression-and-denoise.mkv";
        const auto result = rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend);
        ASSERT_EQ(compression->requests.size(),12U);
        ASSERT_EQ(temporal->timestamps.size(),12U);
        ASSERT_EQ(result.sourceFrames,12U);
        EXPECT_EQ(result.sourceDecodeAcceleration,"software");
        ASSERT_EQ(result.validation.decodedFrames,24U);
        ASSERT_EQ(order.front(),"compression:0");
        const auto firstTemporal = std::find_if(order.begin(),order.end(),[](const std::string& value) {
            return value.rfind("temporal:",0) == 0;
        });
        ASSERT_NE(firstTemporal,order.end());
        EXPECT_TRUE(std::all_of(order.begin(),firstTemporal,[](const std::string& value) {
            return value.rfind("compression:",0) == 0;
        }));
        for (std::size_t index = 0; index < compression->requests.size(); ++index) {
            const auto& request = compression->requests[index];
            EXPECT_EQ(request.contextSize + request.missingBefore + request.missingAfter,7U);
            EXPECT_LE(request.contextSize,7U);
            const auto scene = index < 6 ? 1U : 2U;
            const auto first = index < 6 ? 0U : 6U;
            EXPECT_EQ(request.sceneSerial,scene);
            EXPECT_EQ(request.sceneFirstIndex,first);
            EXPECT_EQ(request.sourceIndex,index);
        }
        const auto original = readExposurePixels(source);
        EXPECT_EQ(temporal->timestamps,original.timestamps);
    }

    void verifyConditioningAndTiming() {
        const auto source = directory / "source.mkv";
        std::mt19937 random(98123);
        std::normal_distribution<double> noise(0,8.0);
        writeExposureSource(source,30,12,6,[&](int index,int,int,int) {
            return index < 6 ? static_cast<int>(std::lround(80 + noise(random))) : 180;
        });
        ExposureLinearBackend backend(*device);
        auto temporal = std::make_shared<RecordingTemporalDenoiseBackend>();
        auto config = settings(); config.motionBlur.enabled = false;
        config.temporalRestorationBackend = temporal;
        config.automaticTemporalDenoise.emplace();
        // This deliberately tiny 64x64 integration fixture has fewer than
        // 1024 independent supports; the production threshold stays at 0.6.
        config.automaticTemporalDenoise->minimumConfidence = 0.2;
        const auto output = directory / "automatic.mkv";
        const auto result = rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend);
        EXPECT_TRUE(result.automaticTemporalDenoiseActive);
        EXPECT_EQ(result.automaticDenoiseAnalyzedFrames,12U);
        EXPECT_EQ(result.automaticDenoiseConfidentFrames,12U);
        EXPECT_EQ(result.automaticDenoiseAppliedFrames,6U);
        const auto report = nlohmann::json::parse(rifelove::application::serializeResult(result));
        const auto& automaticReport = report.at("automaticGaussianDenoise");
        EXPECT_TRUE(automaticReport.at("active").get<bool>());
        EXPECT_EQ(automaticReport.at("analyzedSourceFrames"),12U);
        EXPECT_EQ(automaticReport.at("confidentSourceFrames"),12U);
        EXPECT_EQ(automaticReport.at("conditionedSourceFrames"),6U);
        ASSERT_EQ(temporal->sigmas.size(),12U);
        for (std::size_t index = 0; index < 6; ++index) {
            EXPECT_GT(temporal->sigmas[index],5.0F);
            EXPECT_LT(temporal->sigmas[index],15.0F);
        }
        for (std::size_t index = 6; index < 12; ++index) EXPECT_FLOAT_EQ(temporal->sigmas[index],0.0F);
        const auto original = readExposurePixels(source);
        EXPECT_EQ(temporal->timestamps,original.timestamps);
        EXPECT_EQ(result.sourceFrames,12U);
        EXPECT_EQ(result.sceneCuts,1U);
        EXPECT_EQ(result.validation.decodedFrames,24U);
        const auto automatic = readExposurePixels(output);
        config.automaticTemporalDenoise.reset();
        temporal->sigmas.clear(); temporal->timestamps.clear();
        const auto fixed = directory / "fixed.mkv";
        const auto fixedResult = rifelove::interpolateVideo(source.string(),fixed.string(),config,*device,backend);
        EXPECT_FALSE(fixedResult.automaticTemporalDenoiseActive);
        EXPECT_EQ(fixedResult.automaticDenoiseAnalyzedFrames,0U);
        for (const auto sigma : temporal->sigmas) EXPECT_FLOAT_EQ(sigma,25.0F);
        const auto manual = readExposurePixels(fixed);
        EXPECT_EQ(result.video.endExclusive,fixedResult.video.endExclusive);
        EXPECT_EQ(automatic.timestamps,manual.timestamps);
        EXPECT_EQ(automatic.frames,manual.frames);
    }

    void verifyConfigurationSafety() {
        const auto source = directory / "source.mkv";
        writeExposureSource(source,30,12);
        ExposureLinearBackend backend(*device);
        auto config = settings(); config.motionBlur.enabled = false;
        config.automaticTemporalDenoise.emplace();
        const auto output = directory / "invalid.mkv";
        EXPECT_THROW((void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend),std::invalid_argument);
        expectNoPublication(output);
        config.temporalRestorationBackend = std::make_shared<RecordingTemporalDenoiseBackend>();
        config.checkpoint.emplace();
        EXPECT_THROW((void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend),std::invalid_argument);
        expectNoPublication(output);
        config.automaticTemporalDenoise.reset();
        config.temporalRestorationBackend.reset();
        config.compressionRestorationBackend = std::make_shared<RecordingCompressionBackend>();
        EXPECT_THROW((void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend),std::invalid_argument);
        expectNoPublication(output);
        config.checkpoint.reset();
        auto invalidCompression = std::make_shared<RecordingCompressionBackend>();
        invalidCompression->framesBefore = 2;
        config.compressionRestorationBackend = invalidCompression;
        EXPECT_THROW((void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend),std::invalid_argument);
        expectNoPublication(output);
    }

    void verifyCheckpointReplay() {
        const auto source = directory / "source.mkv";
        std::mt19937 random(73127);
        std::normal_distribution<double> noise(0,1.0);
        writeExposureSource(source,30,12,6,[&](int index,int,int,int) {
            // Vary noise across the first scene so a wrong smoothing state
            // affects both the recorded parameters and generated pixels.
            return index < 6 ? static_cast<int>(std::lround(80 + (6 + index * 2) * noise(random))) : 180;
        });
        ExposureLinearBackend backend(*device);
        auto config = settings(); config.motionBlur.enabled = false;
        config.automaticTemporalDenoise.emplace(); config.automaticTemporalDenoise->minimumConfidence = 0.2;
        const auto checkpoint = [&](const std::string& prefix, std::vector<rifelove::media::VideoSegmentInput>& segments) {
            rifelove::PipelineCheckpoint value;
            value.framesPerSegment = 4;
            value.restorationIdentityVerified = true; // Separate manifest tests bind the parameters.
            value.completedSegments = segments;
            value.segmentPath = [&,prefix](std::uint64_t index) {
                return (directory / (prefix + "-segment-" + std::to_string(index) + ".mkv")).string();
            };
            value.commitSegment = [&segments](std::uint64_t index,std::uint64_t,std::uint64_t,const rifelove::media::VideoSegmentInput& segment) {
                EXPECT_EQ(index,segments.size()); segments.push_back(segment);
            };
            return value;
        };
        std::vector<rifelove::media::VideoSegmentInput> referenceSegments;
        auto referenceTemporal = std::make_shared<RecordingTemporalDenoiseBackend>();
        referenceTemporal->conditioningChangesPixels = true;
        config.temporalRestorationBackend = referenceTemporal;
        config.checkpoint = checkpoint("reference",referenceSegments);
        const auto referencePath = directory / "reference.mkv";
        const auto reference = rifelove::interpolateVideo(source.string(),referencePath.string(),config,*device,backend);
        const auto referencePixels = readExposurePixels(referencePath);
        for (const std::size_t interruptAfter : {10U,18U}) {
            SCOPED_TRACE(interruptAfter);
            const auto prefix = "resumed-" + std::to_string(interruptAfter);
            const auto destination = directory / (prefix + ".mkv");
            std::vector<rifelove::media::VideoSegmentInput> committed;
            auto interruptedTemporal = std::make_shared<RecordingTemporalDenoiseBackend>();
            interruptedTemporal->conditioningChangesPixels = true;
            config.temporalRestorationBackend = interruptedTemporal;
            config.checkpoint = checkpoint(prefix,committed);
            std::stop_source stop;
            EXPECT_ANY_THROW((void)rifelove::interpolateVideo(source.string(),destination.string(),config,*device,backend,
                [&](const rifelove::PipelineProgress& event) {
                    if (event.outputFrames >= interruptAfter) stop.request_stop();
                },stop.get_token()));
            ASSERT_TRUE(stop.stop_requested());
            ASSERT_FALSE(committed.empty());
            ASSERT_FALSE(std::filesystem::exists(destination));
            const auto reused = std::accumulate(committed.begin(),committed.end(),std::size_t{},
                [](std::size_t count,const auto& segment) { return count + segment.contract.frameCount; });
            auto resumedTemporal = std::make_shared<RecordingTemporalDenoiseBackend>();
            resumedTemporal->conditioningChangesPixels = true;
            config.temporalRestorationBackend = resumedTemporal;
            config.checkpoint = checkpoint(prefix,committed);
            const auto resumed = rifelove::interpolateVideo(source.string(),destination.string(),config,*device,backend);
            EXPECT_EQ(resumedTemporal->sigmas,referenceTemporal->sigmas);
            EXPECT_EQ(resumedTemporal->timestamps,referenceTemporal->timestamps);
            EXPECT_EQ(resumed.sourceDecodeAcceleration,"software");
            EXPECT_EQ(resumed.automaticDenoiseAnalyzedFrames,12U);
            EXPECT_EQ(resumed.reusedFrames,reused);
            EXPECT_EQ(resumed.processedFrames + resumed.reusedFrames,24U);
            EXPECT_EQ(resumed.video.endExclusive,reference.video.endExclusive);
            const auto pixels = readExposurePixels(destination);
            EXPECT_EQ(pixels.timestamps,referencePixels.timestamps);
            EXPECT_EQ(pixels.frames,referencePixels.frames);
        }
    }

    void verifyAutomaticCancellation() {
        const auto source = directory / "source.mkv";
        writeExposureSource(source,30,12,6);
        ExposureLinearBackend backend(*device);
        auto config = settings(); config.motionBlur.enabled = false;
        config.temporalRestorationBackend = std::make_shared<RecordingTemporalDenoiseBackend>();
        config.automaticTemporalDenoise.emplace();
        const auto output = directory / "cancelled.mkv";
        std::stop_source stop;
        std::size_t processed = 0;
        try {
            (void)rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend,
                [&](const rifelove::PipelineProgress& progress) {
                    processed = std::max(processed,progress.processedFrames);
                    if (processed >= 2) stop.request_stop();
                },stop.get_token());
            FAIL() << "Cancelled automatic-denoise output was published";
        } catch (const rifelove::inference::InferenceError& error) {
            EXPECT_EQ(error.kind(),rifelove::inference::InferenceErrorKind::cancelled);
        } catch (const rifelove::media::MediaError& error) {
            EXPECT_TRUE(error.cancelled()) << error.what();
        }
        EXPECT_TRUE(stop.stop_requested());
        EXPECT_GE(processed,2U);
        expectNoPublication(output);
    }
};
} // namespace

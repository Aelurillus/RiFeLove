#pragma once

#include "motion_blur_pipeline_test.hpp"
#include "application/job_serialization.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
// Protocol-only fixture: a positive restoration request qualifies its simulated
// provider, while a zero-strength request returns its center without inference.
// This records diagnostic state transitions, not neural restoration quality.
class ProfileRecordingCompressionBackend final : public rifelove::restoration::IVideoRestorationBackend {
public:
    static constexpr const char* pendingVerification =
        "CUDA profile verification is required before the first result is publishable";

    rifelove::restoration::Capabilities capabilities() const override {
        using namespace rifelove::restoration;
        Capabilities result{"test-profile-deblock","stdf-test-v1",1,{Task::deblock},
            {Placement::sourceRateBeforeInterpolation},{"bt709-sdr8"},3,3,1,1,true};
        result.runtimeBackend = "test-cuda-diagnostics";
        result.fallbackReason = qualified ? "" : pendingVerification;
        result.runtimeSignature = "test-stdf-profile-gate-v1";
        return result;
    }

    std::future<rifelove::restoration::Result> restore(rifelove::restoration::Request request) override {
        rifelove::restoration::validateRequest(capabilities(),request);
        if (!request.sceneBounds) throw std::runtime_error("Diagnostic fixture requires source scene bounds");
        strengths.push_back(request.deblockStrength);
        if (request.deblockStrength.value_or(1.0F) > 0.0F) {
            ++inferenceRequests;
            qualified = true;
        }
        std::promise<rifelove::restoration::Result> promise;
        promise.set_value({request.context.at(request.centerIndex),0.0});
        return promise.get_future();
    }

    std::vector<std::optional<float>> strengths;
    std::size_t inferenceRequests = 0;
    bool qualified = false;
};

class RestorationDiagnosticsPipeline : public MotionBlurPipeline {
protected:
    void verifyCompressionDiagnostics(bool automaticBypass) {
        const auto source = directory / "diagnostics-source.mkv";
        // The one-frame source emits its seven-frame, boundary-padded request
        // only when the compression window drains at EOF.
        writeExposureSource(source,30,1,-1,[](int,int,int,int) { return 100; },AV_PIX_FMT_YUV420P);
        for (const bool checkpointMode : {false,true}) {
            SCOPED_TRACE(::testing::Message() << "automaticBypass=" << automaticBypass
                << " checkpoint=" << checkpointMode);
            ExposureLinearBackend backend(*device);
            auto compression = std::make_shared<ProfileRecordingCompressionBackend>();
            ASSERT_EQ(compression->capabilities().fallbackReason,
                ProfileRecordingCompressionBackend::pendingVerification);
            auto config = settings();
            config.motionBlur.enabled = false;
            config.compressionRestorationBackend = compression;
            if (automaticBypass) config.automaticCompressionRestoration.emplace();
            const std::string stem = checkpointMode ? "diagnostics-checkpoint" : "diagnostics-direct";
            const auto output = directory / (stem + ".mkv");
            std::vector<rifelove::media::VideoSegmentInput> segments;
            std::optional<nlohmann::json> publicationResult;
            if (checkpointMode) {
                rifelove::PipelineCheckpoint checkpoint;
                checkpoint.framesPerSegment = 4;
                // Manifest/model identity binding has dedicated tests; this
                // fixture checks the result passed to durable publication.
                checkpoint.restorationIdentityVerified = true;
                checkpoint.segmentPath = [&,stem](std::uint64_t index) {
                    return (directory / (stem + "-segment-" + std::to_string(index) + ".mkv")).string();
                };
                checkpoint.commitSegment = [&](std::uint64_t index,std::uint64_t,std::uint64_t,
                    const rifelove::media::VideoSegmentInput& segment) {
                    EXPECT_EQ(index,segments.size());
                    segments.push_back(segment);
                };
                checkpoint.prepareOutputPublication = [&](const std::string&,const rifelove::PipelineResult& result) {
                    EXPECT_FALSE(publicationResult.has_value());
                    publicationResult = nlohmann::json::parse(rifelove::application::serializeResult(result));
                };
                config.checkpoint = std::move(checkpoint);
            }
            const auto result = rifelove::interpolateVideo(source.string(),output.string(),config,*device,backend);
            ASSERT_EQ(compression->strengths.size(),1U);
            EXPECT_EQ(compression->inferenceRequests,automaticBypass ? 0U : 1U);
            EXPECT_EQ(compression->qualified,!automaticBypass);
            if (automaticBypass) {
                ASSERT_TRUE(compression->strengths.front().has_value());
                EXPECT_FLOAT_EQ(*compression->strengths.front(),0.0F);
                EXPECT_TRUE(result.automaticCompressionRestorationActive);
                EXPECT_EQ(result.automaticCompressionAnalyzedFrames,1U);
                EXPECT_EQ(result.automaticCompressionAppliedFrames,0U);
            } else {
                EXPECT_FALSE(compression->strengths.front().has_value());
                EXPECT_FALSE(result.automaticCompressionRestorationActive);
            }
            const std::string expectedFallback = automaticBypass
                ? ProfileRecordingCompressionBackend::pendingVerification : "";
            EXPECT_EQ(result.compressionRestorationModelId,"test-profile-deblock");
            EXPECT_EQ(result.compressionRestorationBackend,"test-cuda-diagnostics");
            EXPECT_EQ(result.compressionRestorationFallbackReason,expectedFallback);
            EXPECT_EQ(result.validation.decodedFrames,2U);
            EXPECT_TRUE(std::filesystem::exists(output));
            const auto verifySerialized = [&](const nlohmann::json& serialized) {
                EXPECT_EQ(serialized.at("compressionRestorationModelId").get<std::string>(),"test-profile-deblock");
                EXPECT_EQ(serialized.at("compressionRestorationBackend").get<std::string>(),"test-cuda-diagnostics");
                EXPECT_EQ(serialized.at("compressionRestorationFallbackReason").get<std::string>(),expectedFallback);
            };
            verifySerialized(nlohmann::json::parse(rifelove::application::serializeResult(result)));
            if (checkpointMode) {
                ASSERT_FALSE(segments.empty());
                ASSERT_TRUE(publicationResult.has_value());
                verifySerialized(*publicationResult);
            } else {
                EXPECT_FALSE(publicationResult.has_value());
            }
        }
    }
};
} // namespace

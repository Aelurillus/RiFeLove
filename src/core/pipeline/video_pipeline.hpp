#pragma once
#include "inference/interpolation_backend.hpp"
#include "interpolation/scene_analysis.hpp"
#include "media/video_writer.hpp"

namespace rifelove {
struct PipelineSettings {
    PipelineSettings() { encoding.encoder = "auto"; }
    Rational targetRate{60};
    media::VideoEncodingSettings encoding;
    quality::SceneSettings scene;
    std::size_t hostFloatBufferLimit = 512ULL * 1024 * 1024;
};
struct PipelineProgress {
    std::string stage;
    std::size_t outputFrames = 0;
    std::size_t aiFrames = 0;
    std::optional<Rational> timestamp;
    double processingFps = 0;
    std::string detail;
};
struct OutputTimingValidation {
    std::size_t decodedFrames = 0;
    Rational timeBase;
    Rational maximumTimestampError;
    Rational durationError;
    std::optional<Rational> declaredEndError;
    Rational auxiliaryMaximumEndError;
    bool quantized = false;
};
struct PipelineResult {
    media::VideoWriteResult video;
    OutputTimingValidation validation;
    std::size_t sourceFrames;
    std::size_t aiFrames;
    std::size_t originalFrames;
    std::size_t heldFrames;
    std::size_t sceneCuts;
    std::size_t duplicatePairs;
    std::size_t abnormalPairs;
    std::string modelId;
    std::string backend;
    std::string encoderFallbackReason;
    std::string sourceEndEvidence;
    double elapsedSeconds;
    double processingFps;
};
// Services own the selected device/backend. The pipeline owns no UI or vendor
// contexts and retains only adjacent source frames and one pending output.
[[nodiscard]] PipelineResult interpolateVideo(const std::string& source, const std::string& destination,
    const PipelineSettings& settings, gpu::CudaDevice& device, inference::IInterpolationBackend& backend,
    const std::function<void(const PipelineProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove

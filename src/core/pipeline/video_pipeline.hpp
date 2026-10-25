#pragma once
#include "inference/interpolation_backend.hpp"
#include "interpolation/model_adapter.hpp"
#include "interpolation/scene_analysis.hpp"
#include "media/video_writer.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rifelove {
struct PipelineResult;
struct PipelineCheckpoint {
    std::size_t framesPerSegment = 256;
    std::vector<media::VideoSegmentInput> completedSegments;
    std::function<std::string(std::uint64_t)> segmentPath;
    std::function<void(std::uint64_t,std::uint64_t,std::uint64_t,const media::VideoSegmentInput&)> commitSegment;
    std::function<void(const std::string&,const PipelineResult&)> prepareOutputPublication;
};
struct PipelineSettings {
    PipelineSettings() { encoding.encoder = "auto"; }
    Rational targetRate{60};
    // auto: TensorRT first with CUDA fallback; cuda: CUDA provider only;
    // tensorrt: require TensorRT and fail clearly when it cannot initialize.
    std::string inferenceBackend = "auto";
    media::VideoEncodingSettings encoding;
    quality::SceneSettings scene;
    // Runtime-only extension registry; omitted from serialized user settings.
    std::shared_ptr<const interpolation::ModelAdapterRegistry> modelAdapters;
    std::size_t hostFloatBufferLimit = 512ULL * 1024 * 1024;
    // Runtime-only queue integration. It is intentionally omitted from the
    // serialized user job configuration.
    std::optional<PipelineCheckpoint> checkpoint;
};
struct PipelineTimingBreakdown {
    double sourceProbeSeconds = 0;
    double encoderSetupSeconds = 0;
    double sourceDecodeSeconds = 0;
    double sourcePreparationSeconds = 0;
    double sceneAnalysisSeconds = 0;
    double inputUploadSeconds = 0;
    double inferenceWaitSeconds = 0;
    double outputDownloadSeconds = 0;
    double colorRestoreSeconds = 0;
    double frameEncodeSeconds = 0;
    double outputFinalizeSeconds = 0;
    double outputValidationSeconds = 0;
    double pipelineWallSeconds = 0;
};
struct PipelineProgress {
    std::string stage;
    std::size_t outputFrames = 0;
    std::size_t aiFrames = 0;
    std::optional<Rational> timestamp;
    double processingFps = 0;
    std::string detail;
    std::string encoder;
    PipelineTimingBreakdown timings;
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
    std::size_t recoveredTimestamps;
    std::string modelId;
    std::string backend;
    std::string sourceDecoder;
    std::string sourceDecodeAcceleration = "software";
    std::string sourceDecodeFallbackReason;
    std::string adapterId = "rife-sdr-v1";
    int adapterVersion = 1;
    std::string inferenceFallbackReason;
    std::string encoderFallbackReason;
    std::string sourceEndEvidence;
    PipelineTimingBreakdown timings;
    double elapsedSeconds;
    double processingFps;
};
// Services own the selected device/backend. The pipeline owns no UI or vendor
// contexts and retains only adjacent source frames and one pending output.
[[nodiscard]] PipelineResult interpolateVideo(const std::string& source, const std::string& destination,
    const PipelineSettings& settings, gpu::CudaDevice& device, inference::IInterpolationBackend& backend,
    const std::function<void(const PipelineProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove

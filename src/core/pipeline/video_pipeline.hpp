#pragma once
#include "inference/interpolation_backend.hpp"
#include "interpolation/model_adapter.hpp"
#include "interpolation/scene_analysis.hpp"
#include "media/video_writer.hpp"
#include "restoration/video_restoration_backend.hpp"
#include "restoration/noise_estimate.hpp"

#include <functional>
#include <cmath>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <vector>

namespace rifelove {
struct PipelineResult;
struct PipelineCheckpoint {
    std::size_t framesPerSegment = 256;
    // Set only after the application binds and verifies restoration identity.
    bool restorationIdentityVerified = false;
    std::vector<media::VideoSegmentInput> completedSegments;
    std::function<std::string(std::uint64_t)> segmentPath;
    std::function<void(std::uint64_t,std::uint64_t,std::uint64_t,const media::VideoSegmentInput&)> commitSegment;
    std::function<void(const std::string&,const PipelineResult&)> prepareOutputPublication;
};
struct MotionBlurSettings {
    // Temporal exposure synthesis is opt-in. It produces one output frame at
    // the requested target rate from several RIFE sample times.
    bool enabled = false;
    int shutterAngleDegrees = 180;
    int sampleCount = 4;
};
struct PipelineSettings {
    PipelineSettings() { encoding.encoder = "auto"; }
    Rational targetRate{60};
    // auto: TensorRT first with CUDA fallback; cuda: CUDA provider only;
    // tensorrt: require TensorRT and fail clearly when it cannot initialize.
    std::string inferenceBackend = "auto";
    media::VideoEncodingSettings encoding;
    quality::SceneSettings scene;
    MotionBlurSettings motionBlur;
    // Runtime-only extension registry; omitted from serialized user settings.
    std::shared_ptr<const interpolation::ModelAdapterRegistry> modelAdapters;
    // Optional local restoration. Output-rate image models run after RIFE;
    // temporal source-rate models run before interpolation through their
    // declared bounded window. The selected backend/model remains local and
    // is not serialized into ordinary user job settings. When
    // temporalRestorationBackend is empty, a temporal-only legacy backend is
    // inferred from this field for backward compatibility.
    std::shared_ptr<restoration::IVideoRestorationBackend> restorationBackend;
    // Optional source-rate temporal restoration. This is runtime-only and is
    // supplied by the application after validating the temporal model pack.
    // The pipeline deliberately keeps this separate from restorationBackend so
    // temporal denoising and output-rate restoration can run in one task.
    std::shared_ptr<restoration::IVideoRestorationBackend> temporalRestorationBackend;
    // Optional source-rate compression/deblock restoration. When selected it
    // is the first bounded temporal stage and its scene identity is passed to
    // temporalRestorationBackend without re-running cut detection.
    std::shared_ptr<restoration::IVideoRestorationBackend> compressionRestorationBackend;
    // FastDVDnet-style temporal denoisers require an explicit Gaussian-noise
    // conditioning level. It is persisted with queue jobs so a checkpoint is
    // reused only with the same conditioning; it is not a generic quality or
    // compression-restoration setting.
    float temporalDenoiseSigma255 = 25.0F;
    // Experimental opt-in, persisted by job schema 9. Checkpoints bind the
    // analyzer/settings/replay-policy identity and rebuild its state by
    // decoding the original software source timeline from the beginning.
    std::optional<restoration::AutomaticDenoiseSettings> automaticTemporalDenoise;
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
    double restorationSeconds = 0;
    double frameEncodeSeconds = 0;
    double outputFinalizeSeconds = 0;
    double outputValidationSeconds = 0;
    double pipelineWallSeconds = 0;
};
struct PipelineProgress {
    std::string stage;
    std::size_t outputFrames = 0;
    std::size_t aiFrames = 0;
    // outputFrames is the complete output timeline. processedFrames counts
    // frames produced by this execution, while reusedFrames counts verified
    // checkpoint frames that were only carried into the final assembly.
    std::size_t processedFrames = 0;
    std::size_t reusedFrames = 0;
    std::optional<Rational> timestamp;
    double processingFps = 0;
    std::string detail;
    std::string encoder;
    PipelineTimingBreakdown timings;
};
struct OutputTimingValidation {
    std::size_t decodedFrames = 0;
    int width = 0;
    int height = 0;
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
    // outputFrames is the complete output timeline. These two counters make
    // resume throughput explicit: processedFrames is work done by this run;
    // reusedFrames is output already present in verified checkpoint segments.
    std::size_t processedFrames = 0;
    std::size_t reusedFrames = 0;
    // Number of actual RIFE invocations in this execution. Motion-blur
    // exposure sampling can make this larger than the AI output-frame count;
    // after checkpoint resume it counts only work performed by this run.
    std::size_t inferenceCalls = 0;
    // Current execution only: reused checkpoint output is not claimed as
    // newly integrated exposure. Quality holds are counted separately.
    bool motionBlurActive = false;
    std::size_t motionBlurredFrames = 0;
    std::size_t motionBlurFallbackFrames = 0;
    std::map<std::string,std::size_t> motionBlurFallbackReasons;
    std::size_t motionBlurEdgeHeldFrames = 0;
    std::size_t motionBlurPeakWindowFrames = 0;
    std::size_t motionBlurPeakPreparedBytes = 0;
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
    std::string restorationModelId;
    std::string restorationBackend;
    std::string restorationFallbackReason;
    int restorationScaleNumerator = 1;
    int restorationScaleDenominator = 1;
    std::string temporalRestorationModelId;
    std::string temporalRestorationBackend;
    std::string temporalRestorationFallbackReason;
    int temporalRestorationScaleNumerator = 1;
    int temporalRestorationScaleDenominator = 1;
    bool automaticTemporalDenoiseActive = false;
    std::size_t automaticDenoiseAnalyzedFrames = 0;
    std::size_t automaticDenoiseConfidentFrames = 0;
    std::size_t automaticDenoiseAppliedFrames = 0;
    std::string inferenceFallbackReason;
    std::string encoderFallbackReason;
    std::string sourceEndEvidence;
    PipelineTimingBreakdown timings;
    double elapsedSeconds = 0;
    double processingFps = 0;
};

// Return a finite, zero-safe throughput for frames produced by this run.
// A fully resumed task therefore reports 0 FPS instead of claiming that all
// reused output was processed during the current attempt. Saturate rather
// than returning infinity when synthetic or corrupted timing data overflows
// the representable double range.
[[nodiscard]] inline double processingFrameRate(std::size_t processedFrames, double elapsedSeconds) noexcept {
    if (processedFrames == 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0) return 0.0;
    const auto ratio = static_cast<long double>(processedFrames) / static_cast<long double>(elapsedSeconds);
    if (!std::isfinite(ratio)) return std::numeric_limits<double>::max();
    const auto maximum = static_cast<long double>(std::numeric_limits<double>::max());
    return static_cast<double>(ratio > maximum ? maximum : ratio);
}
// Services own the selected device/backend. The pipeline owns no UI or vendor
// contexts. Ordinary interpolation retains adjacent source frames; exposure
// synthesis retains the exact shutter window within the host-buffer budget.
[[nodiscard]] PipelineResult interpolateVideo(const std::string& source, const std::string& destination,
    const PipelineSettings& settings, gpu::CudaDevice& device, inference::IInterpolationBackend& backend,
    const std::function<void(const PipelineProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove

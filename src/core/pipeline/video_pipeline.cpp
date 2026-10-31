#include "core/pipeline/video_pipeline.hpp"
#include "core/pipeline/output_validation.hpp"
#include "core/pipeline/motion_blur.hpp"
#include "common/logging.hpp"
#include "media/nvenc_diagnostic.hpp"
#include "media/rife_color.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/temporal_window.hpp"
#include "timeline/timeline.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <utility>

namespace rifelove {
namespace {
void checkStop(std::stop_token stop) { if (stop.stop_requested()) throw inference::InferenceError(inference::InferenceErrorKind::cancelled,"Video processing cancelled"); }
Rational absolute(Rational value) { return value < Rational(0) ? Rational(0) - value : value; }
Rational encodingTimeBase(Rational sourceBase, Rational targetRate) {
    const auto a = sourceBase.denominator(), b = targetRate.numerator();
    const auto factor = a / std::gcd(a,b);
    if (factor > std::numeric_limits<int>::max() / b) throw std::invalid_argument("Source and target times need an encoder time base beyond 32-bit range");
    return Rational(1,factor * b);
}
void validateTargetDimensions(int referenceWidth, int referenceHeight, int targetWidth, int targetHeight) {
    if ((targetWidth == 0) != (targetHeight == 0) || targetWidth < 0 || targetHeight < 0)
        throw std::invalid_argument("Target output dimensions must be omitted together or both be positive");
    if (targetWidth == 0) return;
    if (referenceWidth <= 0 || referenceHeight <= 0 || targetWidth > 16384 || targetHeight > 16384)
        throw std::invalid_argument("Target output dimensions are outside the supported encoding limit");
    if (targetWidth > referenceWidth || targetHeight > referenceHeight)
        throw std::invalid_argument("Target output dimensions cannot upscale beyond the generated restoration frame");
    if (static_cast<std::int64_t>(targetWidth) * referenceHeight != static_cast<std::int64_t>(targetHeight) * referenceWidth)
        throw std::invalid_argument("Target output dimensions would change the source display aspect ratio");
}
struct CachedFrame { media::DecodedFrame source; media::RifeSdrFrame prepared; std::optional<inference::GpuFrame> gpu; };
class ScopedTiming final {
public:
    explicit ScopedTiming(double& destination) : destination_(destination), started_(std::chrono::steady_clock::now()) {}
    ~ScopedTiming() { destination_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count(); }
    ScopedTiming(const ScopedTiming&) = delete;
    ScopedTiming& operator=(const ScopedTiming&) = delete;
private:
    double& destination_;
    std::chrono::steady_clock::time_point started_;
};
template<class Work> decltype(auto) measure(double& destination, Work&& work) {
    ScopedTiming timing(destination);
    return std::forward<Work>(work)();
}
} // namespace
OutputTimingValidation detail::validatePipelineOutput(const std::string& path, int selected, int expectedWidth, int expectedHeight, Rational origin, Rational end, Rational rate,
    std::size_t expectedCount, const media::MediaInfo& source, const std::function<void(const PipelineProgress&)>& progress, std::stop_token stop) {
    const auto info = media::MediaEngine::probe(path,stop);
    const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const media::StreamInfo& stream) { return stream.index == selected && stream.type == "video"; });
    if (video == info.streams.end() || video->timeBase <= Rational(0)) throw std::runtime_error("Output has no valid video time base");
    if (video->width != expectedWidth || video->height != expectedHeight)
        throw std::runtime_error("Output dimensions do not match the selected processing pipeline");
    const auto originalVideo = std::find_if(source.streams.begin(),source.streams.end(),[&](const media::StreamInfo& stream) {
        return stream.index == selected && stream.type == "video";
    });
    if (originalVideo == source.streams.end()) throw std::runtime_error("Original selected video stream is unavailable for output validation");
    const auto& inputColor = originalVideo->color;
    const auto& outputColor = video->color;
    if (outputColor.pixelFormat != inputColor.pixelFormat || outputColor.bitDepth != inputColor.bitDepth ||
        outputColor.primaries != inputColor.primaries || outputColor.transfer != inputColor.transfer ||
        outputColor.matrix != inputColor.matrix || outputColor.range != inputColor.range)
        throw std::runtime_error("Output pixel format, bit depth or color tags did not preserve the source contract");
    OutputTimingValidation result{}; result.timeBase = video->timeBase; result.width = video->width; result.height = video->height;
    if (video->declaredEnd) {
        result.declaredEndError = *video->declaredEnd - end;
        if (absolute(*result.declaredEndError) > video->timeBase) throw std::runtime_error("Output declared video end " + video->declaredEnd->toString() + " differs from source end " + end.toString());
    }
    for (const auto& incoming : source.streams) {
        if (incoming.index == selected) continue;
        const auto outgoing = std::find_if(info.streams.begin(),info.streams.end(),[&](const media::StreamInfo& stream) { return stream.index == incoming.index; });
        if (outgoing == info.streams.end() || outgoing->type != incoming.type || outgoing->codec != incoming.codec || outgoing->sampleRate != incoming.sampleRate || outgoing->channels != incoming.channels)
            throw std::runtime_error("Copied stream codec/audio layout changed during output");
        if (incoming.declaredEnd) {
            if (!outgoing->declaredEnd) throw std::runtime_error("Copied stream end could not be verified");
            const auto error = absolute(*outgoing->declaredEnd - *incoming.declaredEnd);
            if (error > std::max(incoming.timeBase,outgoing->timeBase)) throw std::runtime_error("Copied stream presentation end changed during output");
            result.auxiliaryMaximumEndError = std::max(result.auxiliaryMaximumEndError,error);
        }
    }
    std::optional<Rational> lastEnd;
    std::optional<Rational> lastDuration;
    const auto scan = media::MediaEngine::decodeVideo(path,[&](const media::DecodedFrame& frame) {
        checkStop(stop);
        if (!frame.timestamp || result.decodedFrames >= expectedCount) throw std::runtime_error("Output timestamp/frame count validation failed");
        const auto expected = origin + Rational(static_cast<std::int64_t>(result.decodedFrames)) / rate;
        const auto error = absolute(*frame.timestamp - expected);
        if (error > video->timeBase / Rational(2)) throw std::runtime_error("Output timestamp differs from scheduled time by more than half a container tick");
        result.maximumTimestampError = std::max(result.maximumTimestampError,error);
        ++result.decodedFrames;
        lastEnd = frame.duration ? std::optional<Rational>(*frame.timestamp + *frame.duration) : std::nullopt;
        lastDuration = frame.duration;
        if (progress && result.decodedFrames % 120 == 0) {
            // outputFrames always names the complete generated timeline. The
            // caller attaches its finished processed/reused counters below;
            // replacing the timeline with this partial scan count would make
            // those counters invalid on every output longer than 120 frames.
            // Report scan progress separately so the UI can still show work
            // advancing while completed output remains at its true frame count.
            const auto detail = "Verified " + std::to_string(result.decodedFrames) + " / " +
                std::to_string(expectedCount) + " output frames";
            progress({"validating",expectedCount,0,0,0,frame.timestamp,0,detail, {},{}});
        }
    },selected,stop);
    if (scan.missingTimestamps || scan.nonIncreasingTimestamps || result.decodedFrames != expectedCount || !lastEnd || !lastDuration || *lastDuration <= Rational(0))
        throw std::runtime_error("Output frame count, ordering or final-frame duration could not be verified");
    result.durationError = *lastEnd - end;
    // Frame PTS and duration can be quantized independently. Every PTS was
    // checked against half a tick above; validate the final duration against
    // its own one-tick bound without hiding the measured combined error.
    const auto lastStart = origin + Rational(static_cast<std::int64_t>(expectedCount - 1)) / rate;
    if (end <= lastStart || absolute(*lastDuration - (end - lastStart)) > video->timeBase)
        throw std::runtime_error("Output final-frame duration differs from the source end by more than one container tick");
    result.quantized = result.maximumTimestampError != Rational(0) || result.durationError != Rational(0) || result.auxiliaryMaximumEndError != Rational(0);
    return result;
}
namespace {
std::optional<std::string> knownColorTag(const std::string& value) {
    if (value.empty() || value == "unknown" || value == "unspecified") return {};
    return value;
}
class LinearLightAccumulator final {
public:
    LinearLightAccumulator(const media::RifeSdrFrame& first, const media::ColorInfo& color,
        std::size_t sampleCount, std::stop_token stop)
        : result_{first.width,first.height,first.paddedWidth,first.paddedHeight,color,
            std::vector<float>(first.pixels.size(),0.0F)},
          weight_(1.0F / static_cast<float>(sampleCount)), stop_(stop), count_(0) {
        if (sampleCount == 0) throw std::invalid_argument("Motion blur requires at least one sample");
    }
    void add(const media::RifeSdrFrame& sample) {
        if (sample.pixels.size() != result_.pixels.size() || sample.width != result_.width ||
            sample.height != result_.height || sample.paddedWidth != result_.paddedWidth || sample.paddedHeight != result_.paddedHeight)
            throw std::invalid_argument("Motion blur samples have mismatched dimensions");
        for (std::size_t index = 0; index < result_.pixels.size(); ++index) {
            if ((index & 0x3FFFFU) == 0) checkStop(stop_);
            const auto value = sample.pixels[index];
            // Match restoreRifeSdr's tolerance for tiny model overshoot while
            // still failing closed for NaN, infinity or material corruption.
            if (!std::isfinite(value) || value < -1e-6F || value > 1.000001F)
                throw std::invalid_argument("Motion blur sample contains invalid normalized RGB values");
            result_.pixels[index] += pipeline::sdrToLinear(std::clamp(value,0.0F,1.0F),result_.sourceColor.transfer) * weight_;
        }
        ++count_;
    }
    media::RifeSdrFrame finish() {
        if (count_ == 0) throw std::invalid_argument("Motion blur produced no samples");
        for (auto& value : result_.pixels) value = std::clamp(pipeline::linearToSdr(value,result_.sourceColor.transfer),0.0F,1.0F);
        return std::move(result_);
    }
private:
    media::RifeSdrFrame result_;
    float weight_;
    std::stop_token stop_;
    std::size_t count_;
};
}

PipelineResult interpolateVideo(const std::string& source, const std::string& destination, const PipelineSettings& settings,
    gpu::CudaDevice& device, inference::IInterpolationBackend& backend,
    const std::function<void(const PipelineProgress&)>& progress, std::stop_token stop) {
    checkStop(stop);
    if (settings.targetRate <= Rational(0)) throw std::invalid_argument("Target frame rate must be positive");
    if (settings.motionBlur.shutterAngleDegrees < 0 || settings.motionBlur.shutterAngleDegrees > 360 ||
        settings.motionBlur.sampleCount < 1 || settings.motionBlur.sampleCount > 16)
        throw std::invalid_argument("Motion blur shutter angle must be 0..360 degrees and sample count must be 1..16");
    const auto capabilities = backend.capabilities();
    if (capabilities.modelId.empty() || !capabilities.dynamicTimestep || capabilities.spatialMultiple != 64)
        throw std::invalid_argument("Selected backend/model does not meet the dynamic RIFE contract");
    const auto adapterRegistry = settings.modelAdapters ? settings.modelAdapters : interpolation::defaultModelAdapterRegistry();
    const auto& adapter = adapterRegistry->resolve(capabilities);
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    PipelineResult result{};
    const auto info = measure(result.timings.sourceProbeSeconds,[&] { return media::MediaEngine::probe(source,stop); });
    auto encoding = settings.encoding;
    if (encoding.videoStreamIndex < 0) encoding.videoStreamIndex = info.preferredVideoStream;
    const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const media::StreamInfo& stream) { return stream.index == encoding.videoStreamIndex && stream.type == "video"; });
    if (video == info.streams.end()) throw std::invalid_argument("Input has no selected video stream");
    if (video->width <= 0 || video->height <= 0 || video->width > 8192 || video->height > 8192)
        throw std::invalid_argument("Input dimensions need a separately supported normalization/tiling profile");
    const auto paddedWidth = static_cast<std::size_t>(((video->width + 63) / 64) * 64);
    const auto paddedHeight = static_cast<std::size_t>(((video->height + 63) / 64) * 64);
    if (paddedWidth * paddedHeight * 3 * sizeof(float) > settings.hostFloatBufferLimit)
        throw std::invalid_argument("Input exceeds the configured host float-buffer limit");
    const auto inputProfile = adapter.inputProfile(video->color);
    const bool legacySdr8 = video->color.bitDepth == 8 &&
        std::find(capabilities.supportedInputProfiles.begin(),capabilities.supportedInputProfiles.end(),"legacy-unverified") != capabilities.supportedInputProfiles.end();
    if (!legacySdr8 && std::find(capabilities.supportedInputProfiles.begin(),capabilities.supportedInputProfiles.end(),inputProfile) == capabilities.supportedInputProfiles.end())
        throw std::invalid_argument("Selected RIFE model does not declare support for input profile " + inputProfile);
    // Resolve the two restoration stages before opening the writer. The
    // application supplies temporalRestorationBackend for the new dual-model
    // path and keeps restorationBackend for the output-rate stage. A legacy
    // single backend is classified by its verified capabilities so old
    // temporal-only jobs continue to work unchanged.
    std::shared_ptr<restoration::IVideoRestorationBackend> temporalBackend = settings.temporalRestorationBackend;
    std::shared_ptr<restoration::IVideoRestorationBackend> outputBackend;
    std::optional<restoration::Capabilities> temporalCapabilities;
    std::optional<restoration::Capabilities> outputRateCapabilities;
    const auto requireRestorationProfile = [&](const restoration::Capabilities& caps) {
        const auto profile = media::rifeSdrInputProfile(video->color);
        if (std::find(caps.supportedInputProfiles.begin(),caps.supportedInputProfiles.end(),profile) == caps.supportedInputProfiles.end())
            throw std::invalid_argument("Selected restoration model does not declare support for input profile " + profile);
    };
    if (settings.restorationBackend) {
        const auto legacyCapabilities = settings.restorationBackend->capabilities();
        restoration::validateCapabilities(legacyCapabilities);
        const bool isTemporal = restoration::supportsOnnxCudaTemporalAdapter(legacyCapabilities);
        const bool isOutputRate = restoration::outputRateImageTask(legacyCapabilities).has_value();
        if (isTemporal == isOutputRate)
            throw std::invalid_argument("The selected restoration model must declare exactly one supported pipeline placement");
        if (isTemporal) {
            if (temporalBackend)
                throw std::invalid_argument("Two temporal restoration backends were selected for the same pipeline stage");
            temporalBackend = settings.restorationBackend;
        } else {
            outputBackend = settings.restorationBackend;
        }
    }
    if (temporalBackend) {
        temporalCapabilities = temporalBackend->capabilities();
        restoration::validateCapabilities(*temporalCapabilities);
        if (!restoration::supportsOnnxCudaTemporalAdapter(*temporalCapabilities))
            throw std::invalid_argument("The temporal restoration backend does not meet the source-rate temporal model contract");
        if (temporalCapabilities->scaleNumerator != 1 || temporalCapabilities->scaleDenominator != 1)
            throw std::invalid_argument("Temporal restoration must preserve the source dimensions");
        requireRestorationProfile(*temporalCapabilities);
        if (!std::isfinite(settings.temporalDenoiseSigma255) || settings.temporalDenoiseSigma255 < 0.0F ||
            settings.temporalDenoiseSigma255 > 55.0F || (settings.temporalDenoiseSigma255 > 0.0F && settings.temporalDenoiseSigma255 < 5.0F))
            throw std::invalid_argument("Temporal restoration noise sigma must be zero or a value from 5 to 55");
    }
    std::optional<restoration::AutomaticDenoiseController> automaticDenoise;
    if (settings.automaticTemporalDenoise) {
        if (!temporalBackend)
            throw std::invalid_argument("Automatic Gaussian denoising requires the FastDVDnet temporal model");
        automaticDenoise.emplace(*settings.automaticTemporalDenoise);
    }
    if (outputBackend) {
        outputRateCapabilities = outputBackend->capabilities();
        restoration::validateCapabilities(*outputRateCapabilities);
        if (!restoration::outputRateImageTask(*outputRateCapabilities))
            throw std::invalid_argument("The output-rate restoration backend does not meet the single-frame image model contract");
        if (outputRateCapabilities->scaleDenominator != 1 || outputRateCapabilities->scaleNumerator < 1 || outputRateCapabilities->scaleNumerator > 4 ||
            video->width > 8192 / outputRateCapabilities->scaleNumerator || video->height > 8192 / outputRateCapabilities->scaleNumerator)
            throw std::invalid_argument("The selected restoration scale exceeds the supported output dimensions");
        requireRestorationProfile(*outputRateCapabilities);
    }
    if ((temporalBackend || outputBackend) && settings.checkpoint && !settings.checkpoint->restorationIdentityVerified)
        throw std::invalid_argument("Checkpoint resume requires verified restoration model and runtime identities");
    if (!encoding.timeBase) encoding.timeBase = encodingTimeBase(video->timeBase,settings.targetRate);
    encoding.frameRate = settings.targetRate;
    const bool explicitOutputDimensions = encoding.outputWidth != 0 || encoding.outputHeight != 0;
    if (outputRateCapabilities) {
        const int generatedWidth = video->width * outputRateCapabilities->scaleNumerator;
        const int generatedHeight = video->height * outputRateCapabilities->scaleNumerator;
        if (explicitOutputDimensions) validateTargetDimensions(generatedWidth,generatedHeight,encoding.outputWidth,encoding.outputHeight);
        else {
            // Preserve the established default: the selected restoration model
            // determines the encoded dimensions when the task has no target.
            encoding.outputWidth = generatedWidth;
            encoding.outputHeight = generatedHeight;
        }
    } else if (explicitOutputDimensions) {
        validateTargetDimensions(video->width,video->height,encoding.outputWidth,encoding.outputHeight);
    }
    const bool checkpointMode = settings.checkpoint.has_value();
    std::vector<media::VideoSegmentInput> segments;
    std::uint64_t completedOutputFrames = 0;
    std::uint64_t nextSegmentIndex = 0;
    if (checkpointMode) {
        const auto& checkpoint = *settings.checkpoint;
        if (checkpoint.framesPerSegment == 0 || !checkpoint.segmentPath || !checkpoint.commitSegment)
            throw std::invalid_argument("Checkpoint mode requires a positive segment size and artifact callbacks");
        segments = checkpoint.completedSegments;
        nextSegmentIndex = segments.size();
        std::optional<std::string> savedEncoder;
        for (std::size_t index = 0; index < segments.size(); ++index) {
            const auto& segment = segments[index];
            const auto count = segment.contract.frameCount;
            if (count == 0 || count > checkpoint.framesPerSegment ||
                (index + 1 < segments.size() && count != checkpoint.framesPerSegment))
                throw std::invalid_argument("Checkpoint segments do not form complete fixed-size boundaries");
            if (segment.contract.frameDuration != Rational(1) / settings.targetRate || segment.contract.encoder.empty())
                throw std::invalid_argument("Checkpoint segment frame rate or encoder identity differs from this task");
            if (segment.contract.frameCount > std::numeric_limits<std::uint64_t>::max() - completedOutputFrames)
                throw std::invalid_argument("Checkpoint output frame count exceeds the supported range");
            if (completedOutputFrames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::invalid_argument("Checkpoint output frame count exceeds exact timestamp range");
            const auto expectedFirst = segments.front().contract.firstTimestamp +
                Rational(static_cast<std::int64_t>(completedOutputFrames)) / settings.targetRate;
            if (segment.contract.firstTimestamp != expectedFirst)
                throw std::invalid_argument("Checkpoint segment timestamp does not match its contiguous output-frame range");
            if (savedEncoder && *savedEncoder != segment.contract.encoder)
                throw std::invalid_argument("Checkpoint segments were produced by different encoders and cannot be safely joined");
            savedEncoder = segment.contract.encoder;
            completedOutputFrames += static_cast<std::uint64_t>(count);
        }
        if (savedEncoder) {
            if (encoding.encoder != "auto" && encoding.encoder != *savedEncoder)
                throw std::invalid_argument("Selected encoder differs from the encoder used by committed checkpoint segments");
            encoding.encoder = *savedEncoder;
        }
        encoding.copyAuxiliaryStreams = false;
        encoding.requireFirstPacketKeyframe = true;
    }
    std::string fallback;
    std::unique_ptr<media::VideoMuxWriter> writer;
    std::string activeSegmentPath;
    if (checkpointMode) activeSegmentPath = settings.checkpoint->segmentPath(nextSegmentIndex);
    const auto initialWriterPath = checkpointMode ? activeSegmentPath : destination;
    const auto encoderSetupStarted = std::chrono::steady_clock::now();
    if (encoding.encoder == "auto") {
        encoding.encoder = "h264_nvenc";
        if (media::MediaEngine::hasEncoder(encoding.encoder)) {
            try { writer = std::make_unique<media::VideoMuxWriter>(source,initialWriterPath,encoding,stop); }
            catch (const media::MediaError& error) {
                if (error.operation() != "open video encoder h264_nvenc" || stop.stop_requested()) throw;
                fallback = media::diagnoseNvencFailure(encoding.encoder,error.what());
            } catch (const std::invalid_argument& error) {
                if (std::string(error.what()).find("encoder does not support the requested pixel format") == std::string::npos) throw;
                fallback = error.what();
            }
        } else fallback = "h264_nvenc is not registered in this FFmpeg build";
        if (!writer) {
            const std::array<const char*,3> softwareEncoders{"libopenh264","libkvazaar","libaom-av1"};
            auto attempts = fallback;
            for (const auto* candidate : softwareEncoders) {
                checkStop(stop);
                const auto capability = media::MediaEngine::probeEncoder(candidate,*video,settings.targetRate,stop);
                checkStop(stop);
                if (!capability.contextOpened) {
                    if (!attempts.empty()) attempts += "; ";
                    attempts += capability.encoder + ": " + capability.reason;
                    continue;
                }
                encoding.encoder = candidate;
                try { writer = std::make_unique<media::VideoMuxWriter>(source,initialWriterPath,encoding,stop); }
                catch (const media::MediaError& error) {
                    if (error.operation() != "open video encoder " + encoding.encoder || stop.stop_requested()) throw;
                    if (!attempts.empty()) attempts += "; ";
                    attempts += error.what();
                    continue;
                } catch (const std::invalid_argument& error) {
                    if (std::string(error.what()).find("encoder does not support the requested pixel format") == std::string::npos) throw;
                    if (!attempts.empty()) attempts += "; ";
                    attempts += encoding.encoder + ": " + error.what();
                    continue;
                }
                fallback = attempts;
                break;
            }
            if (!writer) throw std::runtime_error("Automatic encoding could not open a software encoder compatible with the source format: " + attempts);
            logging::write(logging::Level::warning,"pipeline","encoder.fallback",
                "NVENC initialization failed; automatic encoding selected a software encoder",
                {{"reason",fallback},{"encoder",encoding.encoder}});
            if (progress) {
                PipelineProgress event{"encoder_fallback",0,0,0,0,{},0,fallback,encoding.encoder,{}};
                progress(event);
            }
        }
    }
    if (!writer) writer = std::make_unique<media::VideoMuxWriter>(source,initialWriterPath,encoding,stop);
    const auto outputTickTolerance = writer->outputTimeBase() / Rational(2);
    result.timings.encoderSetupSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - encoderSetupStarted).count();
    result.modelId = capabilities.modelId; result.backend = capabilities.backend;
    result.adapterId = capabilities.adapterId; result.adapterVersion = capabilities.adapterVersion;
    if (outputRateCapabilities) {
        result.restorationModelId = outputRateCapabilities->modelId;
        result.restorationBackend = outputRateCapabilities->runtimeBackend;
        result.restorationFallbackReason = outputRateCapabilities->fallbackReason;
        result.restorationScaleNumerator = outputRateCapabilities->scaleNumerator;
        result.restorationScaleDenominator = outputRateCapabilities->scaleDenominator;
    }
    if (temporalCapabilities) {
        result.temporalRestorationModelId = temporalCapabilities->modelId;
        result.temporalRestorationBackend = temporalCapabilities->runtimeBackend;
        result.temporalRestorationFallbackReason = temporalCapabilities->fallbackReason;
        result.temporalRestorationScaleNumerator = temporalCapabilities->scaleNumerator;
        result.temporalRestorationScaleDenominator = temporalCapabilities->scaleDenominator;
        // Preserve the old result contract for callers that only understand a
        // single temporal restoration backend.
        if (!outputRateCapabilities) {
            result.restorationModelId = result.temporalRestorationModelId;
            result.restorationBackend = result.temporalRestorationBackend;
            result.restorationFallbackReason = result.temporalRestorationFallbackReason;
            result.restorationScaleNumerator = result.temporalRestorationScaleNumerator;
            result.restorationScaleDenominator = result.temporalRestorationScaleDenominator;
        }
    }
    result.inferenceFallbackReason = capabilities.fallbackReason; result.encoderFallbackReason = fallback;
    std::optional<CachedFrame> left;
    std::optional<CachedFrame> right;
    std::optional<quality::SceneAnalysis> pairAnalysis;
    std::optional<CfrClock> clock;
    std::optional<media::DecodedFrame> pending;
    std::optional<Rational> pendingTime;
    std::optional<Rational> origin;
    std::shared_ptr<gpu::DeviceBuffer> reusableOutput;
    gpu::DeviceBufferPool inputBuffers(device,2);
    const bool motionBlurActive = settings.motionBlur.enabled && settings.motionBlur.shutterAngleDegrees > 0 && settings.motionBlur.sampleCount > 1;
    result.motionBlurActive = motionBlurActive;
    // Source-window storage is bounded by both the configured aggregate
    // prepared-float budget and an absolute frame-count cap. If a shutter
    // cannot fit, fail explicitly instead of silently outputting sharp frames.
    const auto preparedFrameBytes = paddedWidth * paddedHeight * 3 * sizeof(float);
    const auto blurWindowFrameLimit = std::min<std::size_t>(4096,settings.hostFloatBufferLimit / preparedFrameBytes);
    std::deque<CachedFrame> blurFrames;
    std::deque<quality::SceneAnalysis> blurAnalyses;
    bool blurBeginsAtSourceStart = true;
    std::optional<restoration::TemporalWindow> temporalWindow;
    if (temporalCapabilities) temporalWindow.emplace(*temporalCapabilities);
    result.automaticTemporalDenoiseActive = automaticDenoise.has_value();
    struct SourceNoiseObservation {
        std::size_t index;
        restoration::GaussianNoiseEstimate estimate;
        bool sceneCut;
    };
    std::deque<SourceNoiseObservation> sourceNoise;
    const auto notify = [&](const std::string& stage, std::optional<Rational> time, const std::string& detail = {}) {
        if (progress) {
            const double seconds = elapsed();
            PipelineProgress event{stage,clock ? clock->index() : 0,result.aiFrames,result.processedFrames,result.reusedFrames,time,
                processingFrameRate(result.processedFrames,seconds),detail,encoding.encoder,result.timings};
            progress(event);
        }
    };
    std::size_t segmentFrameCount = 0;
    std::uint64_t segmentFirstFrame = completedOutputFrames;
    std::uint64_t segmentEndFrame = completedOutputFrames;
    Rational segmentFirstTimestamp;
    Rational segmentEndExclusive;
    const auto openNextSegment = [&] {
        if (!checkpointMode || writer) return;
        activeSegmentPath = settings.checkpoint->segmentPath(nextSegmentIndex);
        writer = std::make_unique<media::VideoMuxWriter>(source,activeSegmentPath,encoding,stop);
    };
    const auto finalizeSegment = [&] {
        if (!checkpointMode || !writer || segmentFrameCount == 0)
            throw std::logic_error("Checkpoint segment cannot be finalized before it receives frames");
        std::optional<media::VideoSegmentContract> verifiedContract;
        (void)writer->finish([&](const std::string& temporary) {
            const auto segmentInfo = media::MediaEngine::probe(temporary,stop);
            const auto stream = std::find_if(segmentInfo.streams.begin(),segmentInfo.streams.end(),
                [](const media::StreamInfo& item) { return item.type == "video"; });
            if (stream == segmentInfo.streams.end()) throw std::runtime_error("Encoded checkpoint segment has no video stream");
            media::VideoSegmentContract contract;
            contract.frameCount = segmentFrameCount;
            contract.firstTimestamp = segmentFirstTimestamp;
            contract.frameDuration = Rational(1) / settings.targetRate;
            contract.endExclusive = segmentEndExclusive;
            contract.width = stream->width; contract.height = stream->height;
            contract.codec = stream->codec; contract.encoder = encoding.encoder;
            contract.pixelFormat = stream->color.pixelFormat; contract.bitDepth = stream->color.bitDepth;
            contract.primaries = knownColorTag(stream->color.primaries);
            contract.transfer = knownColorTag(stream->color.transfer);
            contract.matrix = knownColorTag(stream->color.matrix);
            contract.range = knownColorTag(stream->color.range);
            (void)media::MediaEngine::verifyVideoSegment(temporary,contract,stop);
            verifiedContract = std::move(contract);
        });
        writer.reset();
        if (!verifiedContract) throw std::runtime_error("Checkpoint segment validation did not produce a media contract");
        media::VideoSegmentInput segment{activeSegmentPath,std::move(*verifiedContract)};
        settings.checkpoint->commitSegment(nextSegmentIndex,segmentFirstFrame,segmentEndFrame,segment);
        segments.push_back(std::move(segment));
        ++nextSegmentIndex;
        segmentFirstFrame = segmentEndFrame;
        segmentFrameCount = 0;
    };
    const auto writePending = [&](const media::DecodedFrame& frame, Rational timestamp, Rational duration) {
        if (!checkpointMode) {
            writer->writeFrame(frame,timestamp,duration);
            return;
        }
        openNextSegment();
        if (segmentFrameCount == 0) segmentFirstTimestamp = timestamp;
        writer->writeFrame(frame,timestamp,duration);
        ++segmentFrameCount;
        ++segmentEndFrame;
        segmentEndExclusive = timestamp + duration;
        if (segmentFrameCount == settings.checkpoint->framesPerSegment) finalizeSegment();
    };
    const auto emit = [&](media::DecodedFrame frame) {
        const auto time = clock->timestamp();
        if (outputBackend) {
            frame.index = clock->index();
            frame.timestamp = time;
            frame.duration = Rational(1) / settings.targetRate;
            restoration::Request request;
            request.task = *restoration::outputRateImageTask(*outputRateCapabilities);
            request.placement = restoration::Placement::outputRateAfterInterpolation;
            request.context.push_back(frame);
            request.centerIndex = 0;
            request.inputProfile = media::rifeSdrInputProfile(frame.color);
            request.stop = stop;
            notify("restoration",time,outputRateCapabilities->modelId);
            frame = measure(result.timings.restorationSeconds,[&] {
                return outputBackend->restore(std::move(request)).get().centerFrame;
            });
        }
        if (pending) measure(result.timings.frameEncodeSeconds,[&] { writePending(*pending,*pendingTime,time - *pendingTime); });
        pending = std::move(frame); pendingTime = time;
        clock->advance();
        if (result.processedFrames == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("Processed output frame counter exceeds the supported range");
        ++result.processedFrames;
        notify("processing",time);
    };
    const auto upload = [&](CachedFrame& frame) -> const inference::GpuFrame& {
        if (!frame.gpu) {
            measure(result.timings.inputUploadSeconds,[&] {
                auto storage = inputBuffers.acquire(frame.prepared.pixels.size() * sizeof(float),stop);
                storage->upload(std::as_bytes(std::span(frame.prepared.pixels)),0,stop);
                frame.gpu = inference::GpuFrame{std::move(storage),frame.prepared.paddedWidth,frame.prepared.paddedHeight};
            });
        }
        return *frame.gpu;
    };
    const auto uploadForSample = [&](CachedFrame& frame) -> inference::GpuFrame {
        // Motion blur keeps a bounded source window. Use short-lived uploads
        // for samples so retaining the host frames does not pin the window's
        // device buffers in the two-buffer input pool.
        if (!motionBlurActive) return upload(frame);
        auto storage = measure(result.timings.inputUploadSeconds,[&] {
            return inputBuffers.acquire(frame.prepared.pixels.size() * sizeof(float),stop);
        });
        measure(result.timings.inputUploadSeconds,[&] {
            storage->upload(std::as_bytes(std::span(frame.prepared.pixels)),0,stop);
        });
        return inference::GpuFrame{std::move(storage),frame.prepared.paddedWidth,frame.prepared.paddedHeight};
    };
    const auto interpolateSample = [&](CachedFrame& firstFrame, CachedFrame& secondFrame, Rational sampleTime) {
        const auto time = (sampleTime - *firstFrame.source.timestamp) / (*secondFrame.source.timestamp - *firstFrame.source.timestamp);
        auto first = uploadForSample(firstFrame); auto second = uploadForSample(secondFrame);
        if (!reusableOutput) reusableOutput = device.allocate(first.pixels->size(),stop);
        auto interpolated = measure(result.timings.inferenceWaitSeconds,[&] {
            return backend.interpolate({first,second,static_cast<float>(time.toLongDouble()),std::move(reusableOutput),stop}).get();
        });
        media::RifeSdrFrame output{};
        output.width = firstFrame.prepared.width; output.height = firstFrame.prepared.height;
        output.paddedWidth = firstFrame.prepared.paddedWidth; output.paddedHeight = firstFrame.prepared.paddedHeight;
        output.sourceColor = firstFrame.prepared.sourceColor;
        output.pixels.resize(firstFrame.prepared.pixels.size());
        measure(result.timings.outputDownloadSeconds,[&] {
            interpolated.frame.pixels->download(std::as_writable_bytes(std::span(output.pixels)),0,stop);
        });
        reusableOutput = device.reclaim(std::move(interpolated.frame.pixels));
        ++result.inferenceCalls;
        return output;
    };
    const auto analyzePair = [&](CachedFrame& firstFrame, CachedFrame& secondFrame) {
        if (!secondFrame.source.timestamp || *secondFrame.source.timestamp <= *firstFrame.source.timestamp) throw std::runtime_error("Source timestamps must be present and strictly increasing");
        if (secondFrame.source.width != firstFrame.source.width || secondFrame.source.height != firstFrame.source.height || secondFrame.source.color.pixelFormat != firstFrame.source.color.pixelFormat ||
            secondFrame.source.color.primaries != firstFrame.source.color.primaries || secondFrame.source.color.transfer != firstFrame.source.color.transfer ||
            secondFrame.source.color.matrix != firstFrame.source.color.matrix || secondFrame.source.color.range != firstFrame.source.color.range)
            throw std::runtime_error("Midstream resolution/format/color changes require an explicit normalization policy");
        const auto analysis = measure(result.timings.sceneAnalysisSeconds,[&] {
            return quality::analyzePair(adapter.analysisView(firstFrame.prepared),adapter.analysisView(secondFrame.prepared),settings.scene);
        });
        if (analysis.action == quality::PairAction::sceneCutHold) ++result.sceneCuts;
        else if (analysis.action == quality::PairAction::duplicateHold) ++result.duplicatePairs;
        else if (analysis.action == quality::PairAction::abnormalMotionHold) ++result.abnormalPairs;
        return analysis;
    };
    const auto blurTimestamps = [&] {
        std::vector<Rational> timestamps;
        timestamps.reserve(blurFrames.size());
        for (const auto& frame : blurFrames) timestamps.push_back(*frame.source.timestamp);
        return timestamps;
    };
    const auto pruneBlurWindow = [&] {
        if (blurFrames.empty()) return;
        const auto timestamps = blurTimestamps();
        const auto [start,end] = pipeline::motionBlurExposureBounds(clock->timestamp(),settings.targetRate,settings.motionBlur.shutterAngleDegrees);
        (void)end;
        auto discard = pipeline::motionBlurDiscardableFrames(timestamps,start);
        while (discard > 0) {
            blurFrames.pop_front(); blurAnalyses.pop_front(); blurBeginsAtSourceStart = false;
            --discard;
        }
    };
    const auto processBlurWindow = [&](bool endOfSource, std::optional<Rational> endExclusive) {
        while (!blurFrames.empty() && (!endExclusive || clock->timestamp() < *endExclusive)) {
            checkStop(stop);
            const auto center = clock->timestamp();
            const auto timestamps = blurTimestamps();
            std::vector<quality::PairAction> actions;
            actions.reserve(blurAnalyses.size());
            for (const auto& analysis : blurAnalyses) actions.push_back(analysis.action);
            const auto plan = pipeline::planMotionBlurExposure(timestamps,actions,center,settings.targetRate,
                settings.motionBlur.shutterAngleDegrees,settings.motionBlur.sampleCount,blurBeginsAtSourceStart,endOfSource);
            if (!plan.ready) break;
            if (plan.center.blocked || center > timestamps.back()) ++result.heldFrames;
            else if (plan.center.sourceFrame) ++result.originalFrames;
            else ++result.aiFrames;
            if (checkpointMode && clock->index() < completedOutputFrames) {
                if (result.reusedFrames == std::numeric_limits<std::size_t>::max())
                    throw std::overflow_error("Reused output frame counter exceeds the supported range");
                ++result.reusedFrames; clock->advance();
                notify("resuming",center,"Reusing a verified checkpoint segment");
            } else if (!plan.fallbackReason.empty()) {
                ++result.motionBlurFallbackFrames;
                ++result.motionBlurFallbackReasons[plan.fallbackReason];
                notify("quality",center,"Motion blur center hold: " + plan.fallbackReason);
                if (plan.center.blocked) emit(blurFrames[plan.center.leftIndex].source);
                else if (plan.center.sourceFrame) emit(blurFrames[plan.center.sourceIndex].source);
                else {
                    auto restored = measure(result.timings.colorRestoreSeconds,[&] {
                        return adapter.postprocess(interpolateSample(blurFrames[plan.center.leftIndex],blurFrames[plan.center.rightIndex],center),stop);
                    });
                    emit(std::move(restored));
                }
            } else {
                LinearLightAccumulator accumulator(blurFrames.front().prepared,blurFrames.front().prepared.sourceColor,plan.samples.size(),stop);
                for (const auto& location : plan.samples) {
                    checkStop(stop);
                    if (location.sourceFrame) accumulator.add(blurFrames[location.sourceIndex].prepared);
                    else {
                        auto& first = blurFrames[location.leftIndex]; auto& second = blurFrames[location.rightIndex];
                        const auto time = *first.source.timestamp + (*second.source.timestamp - *first.source.timestamp) * location.timestep;
                        accumulator.add(interpolateSample(first,second,time));
                    }
                }
                auto integrated = accumulator.finish();
                auto restored = measure(result.timings.colorRestoreSeconds,[&] { return adapter.postprocess(integrated,stop); });
                ++result.motionBlurredFrames;
                if (plan.edgeHeld) ++result.motionBlurEdgeHeldFrames;
                emit(std::move(restored));
            }
            pruneBlurWindow();
        }
        // Also prune between output ticks, while decoding toward the next
        // shutter. This keeps memory bounded when downsampling dense sources.
        pruneBlurWindow();
    };
    const auto pair = [&](CachedFrame& firstFrame, const quality::SceneAnalysis& analysis, CachedFrame& secondFrame) {
        if (analysis.action != quality::PairAction::interpolate) notify("quality",secondFrame.source.timestamp,std::string(quality::toString(analysis.action)));
        while (clock->timestamp() < *secondFrame.source.timestamp) {
            checkStop(stop);
            const auto leftDelta = absolute(clock->timestamp() - *firstFrame.source.timestamp);
            const auto rightDelta = absolute(clock->timestamp() - *secondFrame.source.timestamp);
            const media::DecodedFrame* coincident = nullptr;
            if (leftDelta <= outputTickTolerance && leftDelta <= rightDelta) coincident = &firstFrame.source;
            else if (rightDelta <= outputTickTolerance) coincident = &secondFrame.source;
            if (checkpointMode && clock->index() < completedOutputFrames) {
                if (analysis.action != quality::PairAction::interpolate) ++result.heldFrames;
                else if (coincident != nullptr) ++result.originalFrames;
                else ++result.aiFrames;
                if (result.reusedFrames == std::numeric_limits<std::size_t>::max())
                    throw std::overflow_error("Reused output frame counter exceeds the supported range");
                ++result.reusedFrames;
                const auto timestamp = clock->timestamp();
                clock->advance();
                notify("resuming",timestamp,"Reusing a verified checkpoint segment");
                continue;
            }
            if (analysis.action != quality::PairAction::interpolate) { ++result.heldFrames; emit(firstFrame.source); }
            else if (coincident) { ++result.originalFrames; emit(*coincident); }
            else {
                ++result.aiFrames;
                auto generatedSample = interpolateSample(firstFrame,secondFrame,clock->timestamp());
                auto restored = measure(result.timings.colorRestoreSeconds,[&] { return adapter.postprocess(generatedSample,stop); });
                emit(std::move(restored));
            }
        }
    };
    const auto processSourceFrame = [&](media::DecodedFrame frame) {
        if (!frame.timestamp) throw std::runtime_error("Interpolation requires a known timestamp for every source frame");
        if (motionBlurActive && blurFrames.size() >= blurWindowFrameLimit)
            throw std::runtime_error("Motion blur full shutter exceeds the bounded source-window memory budget; lower the shutter angle or increase hostFloatBufferLimit");
        auto prepared = measure(result.timings.sourcePreparationSeconds,[&] { return adapter.preprocess(frame,settings.hostFloatBufferLimit,stop); });
        CachedFrame current{std::move(frame),std::move(prepared),{}};
        if (!clock) {
            origin = current.source.timestamp; clock.emplace(*origin,settings.targetRate);
            if (!segments.empty() && segments.front().contract.firstTimestamp != *origin)
                throw std::runtime_error("Checkpoint source start timestamp changed; committed segments cannot be resumed");
        }
        if (motionBlurActive) {
            if (!blurFrames.empty()) blurAnalyses.push_back(analyzePair(blurFrames.back(),current));
            blurFrames.push_back(std::move(current));
            result.motionBlurPeakWindowFrames = std::max(result.motionBlurPeakWindowFrames,blurFrames.size());
            result.motionBlurPeakPreparedBytes = std::max(result.motionBlurPeakPreparedBytes,blurFrames.size() * preparedFrameBytes);
            processBlurWindow(false,{});
            return;
        }
        if (!left) {
            left = std::move(current);
        } else if (!right) {
            right = std::move(current);
            pairAnalysis = analyzePair(*left,*right);
        } else {
            auto nextAnalysis = analyzePair(*right,current);
            pair(*left,*pairAnalysis,*right);
            left = std::move(*right);
            right = std::move(current);
            pairAnalysis = std::move(nextAnalysis);
        }
    };
    // Temporal models need a scene boundary before their five-frame request is
    // assembled. Use the same analysis policy on the real source frames, but
    // keep its counters attached to the post-restoration interpolation stream.
    std::optional<media::RifeSdrFrame> temporalPreviousPrepared;
    const auto temporalSceneCutBefore = [&](const media::DecodedFrame& frame) {
        if (!temporalWindow) return false;
        auto prepared = media::prepareRifeSdr(frame,settings.hostFloatBufferLimit,stop);
        bool cut = false;
        if (temporalPreviousPrepared) {
            const auto analysis = quality::analyzePair(adapter.analysisView(*temporalPreviousPrepared),adapter.analysisView(prepared),settings.scene);
            cut = analysis.action == quality::PairAction::sceneCutHold;
        }
        if (automaticDenoise) {
            auto estimate = measure(result.timings.sceneAnalysisSeconds,[&] { return restoration::estimateGaussianNoise(prepared,stop); });
            sourceNoise.push_back({frame.index,estimate,cut});
            ++result.automaticDenoiseAnalyzedFrames;
        }
        temporalPreviousPrepared = std::move(prepared);
        return cut;
    };
    const auto processTemporalRequests = [&](std::vector<restoration::Request> requests) {
        for (auto& request : requests) {
            request.stop = stop;
            request.denoiseSigma255 = settings.temporalDenoiseSigma255;
            if (automaticDenoise) {
                const auto& center = request.context[request.centerIndex];
                if (sourceNoise.empty() || sourceNoise.front().index != center.index)
                    throw std::logic_error("Automatic denoise observation does not match the source-frame window");
                const auto value = automaticDenoise->update(*center.timestamp,sourceNoise.front().estimate,sourceNoise.front().sceneCut);
                request.denoiseSigma255 = value.sigma255;
                if (value.confident) ++result.automaticDenoiseConfidentFrames;
                if (value.sigma255 > 0.0F) ++result.automaticDenoiseAppliedFrames;
                sourceNoise.pop_front();
            }
            auto restored = measure(result.timings.restorationSeconds,[&] {
                return temporalBackend->restore(std::move(request)).get().centerFrame;
            });
            processSourceFrame(std::move(restored));
        }
    };
    notify("decoding",{});
    double decodeCallbackSeconds = 0;
    const auto decodeStarted = std::chrono::steady_clock::now();
    const auto decoded = media::MediaEngine::decodeVideo(source,[&](const media::DecodedFrame& frame) {
        ScopedTiming callbackTiming(decodeCallbackSeconds);
        checkStop(stop);
        if (!temporalWindow) processSourceFrame(frame);
        else processTemporalRequests(temporalWindow->push(frame,temporalSceneCutBefore(frame)));
    // Automatic sigma must replay the identical source samples before a
    // checkpoint boundary. Bind this software decode policy in job identity
    // rather than allowing a later NVDEC/software fallback to change samples.
    // decodeVideo starts at the beginning even when output segments are reused.
    },encoding.videoStreamIndex,stop,{automaticDenoise ? media::DecodeAcceleration::software : media::DecodeAcceleration::preferCuda,device.info().ordinal});
    const auto decodeWallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - decodeStarted).count();
    result.timings.sourceDecodeSeconds = std::max(0.0,decodeWallSeconds - decodeCallbackSeconds);
    result.sourceFrames = decoded.frameCount;
    result.recoveredTimestamps = decoded.recoveredTimestamps;
    result.sourceDecoder = decoded.decoder;
    result.sourceDecodeAcceleration = decoded.acceleration;
    result.sourceDecodeFallbackReason = decoded.accelerationFallbackReason;
    if (temporalWindow) processTemporalRequests(temporalWindow->finish());
    if (!sourceNoise.empty()) throw std::logic_error("Automatic denoise observations were not fully drained");
    if (motionBlurActive ? blurFrames.empty() : !left) throw std::runtime_error("Source produced no video frames");
    if (!motionBlurActive && right) pair(*left,*pairAnalysis,*right);
    // Copy the decoder reference: draining/pruning the shutter window below
    // may invalidate its deque element, but the true final boundary is fixed.
    const auto last = motionBlurActive ? blurFrames.back().source : (right ? right->source : left->source);
    std::optional<Rational> boundary;
    if (last.duration && *last.duration > Rational(0)) boundary = *last.timestamp + *last.duration;
    result.sourceEndEvidence = "last_frame_duration";
    if (video->declaredEnd) {
        if (*video->declaredEnd <= *last.timestamp || (boundary && absolute(*video->declaredEnd - *boundary) > video->timeBase))
            throw std::runtime_error("Declared video end contradicts the final frame boundary; explicit source normalization is required");
        boundary = video->declaredEnd; result.sourceEndEvidence = "declared_stream_end";
    }
    if (!boundary) throw std::runtime_error("Final source frame and stream lack a reliable end; cannot preserve duration");
    const auto end = *boundary;
    if (motionBlurActive) processBlurWindow(true,end);
    else while (clock->timestamp() < end) {
        checkStop(stop);
        const auto coincident = absolute(clock->timestamp() - *last.timestamp) <= outputTickTolerance;
        if (coincident) ++result.originalFrames;
        else ++result.heldFrames;
        if (checkpointMode && clock->index() < completedOutputFrames) {
            if (result.reusedFrames == std::numeric_limits<std::size_t>::max())
                throw std::overflow_error("Reused output frame counter exceeds the supported range");
            ++result.reusedFrames;
            const auto timestamp = clock->timestamp();
            clock->advance();
            notify("resuming",timestamp,"Reusing a verified checkpoint segment");
            continue;
        }
        emit(last);
    }
    if (motionBlurActive && result.motionBlurredFrames + result.motionBlurFallbackFrames != result.processedFrames)
        throw std::logic_error("Motion blur exposure accounting does not match the newly processed output");
    if (!origin || end <= *origin) throw std::runtime_error("No valid final output frame boundary");
    if (!checkpointMode && (!pending || !pendingTime || end <= *pendingTime))
        throw std::runtime_error("No valid final output frame boundary");
    if (pending && pendingTime)
        measure(result.timings.frameEncodeSeconds,[&] { writePending(*pending,*pendingTime,end - *pendingTime); });
    notify("validating",pendingTime.value_or(end));
    const auto finishStarted = std::chrono::steady_clock::now();
    const auto validateTemporary = [&](const std::string& temporary) {
        result.validation = measure(result.timings.outputValidationSeconds,[&] {
            const auto validationStarted = std::chrono::steady_clock::now();
            const auto validationProgress = [&](const PipelineProgress& sourceProgress) {
                if (!progress) return;
                auto event = sourceProgress;
                event.timings = result.timings;
                event.timings.outputValidationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - validationStarted).count();
                event.processedFrames = result.processedFrames;
                event.reusedFrames = result.reusedFrames;
                event.processingFps = processingFrameRate(result.processedFrames,elapsed());
                progress(event);
            };
            return detail::validatePipelineOutput(temporary,encoding.videoStreamIndex,encoding.outputWidth > 0 ? encoding.outputWidth : video->width,
                encoding.outputHeight > 0 ? encoding.outputHeight : video->height,*origin,end,settings.targetRate,clock->index(),info,validationProgress,stop);
        });
    };
    if (checkpointMode) {
        if (segmentFrameCount > 0) finalizeSegment();
        else if (writer) writer.reset(); // discard the empty preflight writer when all frames were already checkpointed
        if (completedOutputFrames > clock->index())
            throw std::runtime_error("Verified checkpoint contains more frames than the source timeline");
        std::uint64_t assembledFrames = 0;
        for (const auto& segment : segments) {
            if (segment.contract.frameCount > std::numeric_limits<std::uint64_t>::max() - assembledFrames)
                throw std::runtime_error("Checkpoint frame count exceeds the supported range");
            assembledFrames += static_cast<std::uint64_t>(segment.contract.frameCount);
        }
        if (assembledFrames != clock->index() || segments.empty())
            throw std::runtime_error("Checkpoint segments do not cover the complete output timeline");
        if (!segments.back().contract.endExclusive || absolute(*segments.back().contract.endExclusive - end) > video->timeBase)
            throw std::runtime_error("Final checkpoint segment does not end at the verified source boundary");
        const auto preparePublication = [&](const std::string& temporary, const media::VideoSegmentAssemblyResult& assembly) {
            const auto assemblySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - finishStarted).count();
            result.video = {encoding.encoder,static_cast<std::size_t>(clock->index()),assembly.videoPackets,assembly.auxiliaryPackets,
                assembly.firstTimestamp,assembly.endExclusive,assemblySeconds,
                processingFrameRate(result.processedFrames,assemblySeconds)};
            result.timings.outputFinalizeSeconds = std::max(0.0,assemblySeconds - result.timings.outputValidationSeconds);
            result.elapsedSeconds = elapsed();
            result.processingFps = processingFrameRate(result.processedFrames,result.elapsedSeconds);
            result.timings.pipelineWallSeconds = result.elapsedSeconds;
            if (settings.checkpoint->prepareOutputPublication)
                settings.checkpoint->prepareOutputPublication(temporary,result);
        };
        const auto assembly = media::MediaEngine::assembleVideoSegments(source,destination,encoding.videoStreamIndex,
            segments,stop,validateTemporary,preparePublication);
        const auto assemblySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - finishStarted).count();
        result.video = {encoding.encoder,static_cast<std::size_t>(clock->index()),assembly.videoPackets,assembly.auxiliaryPackets,
            assembly.firstTimestamp,assembly.endExclusive,assemblySeconds,
            processingFrameRate(result.processedFrames,assemblySeconds)};
    } else {
        result.video = writer->finish(validateTemporary);
    }
    const auto finishWallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - finishStarted).count();
    result.timings.outputFinalizeSeconds = std::max(0.0,finishWallSeconds - result.timings.outputValidationSeconds);
    result.sourceFrames = decoded.frameCount;
    result.recoveredTimestamps = decoded.recoveredTimestamps;
    result.elapsedSeconds = elapsed();
    result.processingFps = processingFrameRate(result.processedFrames,result.elapsedSeconds);
    if (!checkpointMode && result.video.elapsedSeconds >= 0)
        result.video.processingFps = processingFrameRate(result.processedFrames,result.video.elapsedSeconds);
    result.timings.pipelineWallSeconds = result.elapsedSeconds;
    logging::write(logging::Level::info,"pipeline","processing.completed","Output passed media and timeline validation",
        {{"encoder",result.video.encoder},{"output_frames",static_cast<std::uint64_t>(result.video.framesSubmitted)},
            {"processed_frames",static_cast<std::uint64_t>(result.processedFrames)},
            {"reused_frames",static_cast<std::uint64_t>(result.reusedFrames)},
            {"rife_frames",static_cast<std::uint64_t>(result.aiFrames)},{"processing_fps",result.processingFps}});
    notify("completed",end);
    return result;
}
} // namespace rifelove

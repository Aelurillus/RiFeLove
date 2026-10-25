#include "core/pipeline/video_pipeline.hpp"
#include "media/rife_color.hpp"
#include "timeline/timeline.hpp"
#include <algorithm>
#include <array>
#include <chrono>
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
quality::RgbView view(const media::RifeSdrFrame& frame) {
    return {frame.pixels,frame.width,frame.height,frame.paddedWidth,static_cast<std::size_t>(frame.paddedWidth) * static_cast<std::size_t>(frame.paddedHeight)};
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
OutputTimingValidation validateOutput(const std::string& path, int selected, Rational origin, Rational end, Rational rate,
    std::size_t expectedCount, const media::MediaInfo& source, const std::function<void(const PipelineProgress&)>& progress, std::stop_token stop) {
    const auto info = media::MediaEngine::probe(path,stop);
    const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const media::StreamInfo& stream) { return stream.index == selected && stream.type == "video"; });
    if (video == info.streams.end() || video->timeBase <= Rational(0)) throw std::runtime_error("Output has no valid video time base");
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
    OutputTimingValidation result{}; result.timeBase = video->timeBase;
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
    const auto scan = media::MediaEngine::decodeVideo(path,[&](const media::DecodedFrame& frame) {
        checkStop(stop);
        if (!frame.timestamp || result.decodedFrames >= expectedCount) throw std::runtime_error("Output timestamp/frame count validation failed");
        const auto expected = origin + Rational(static_cast<std::int64_t>(result.decodedFrames)) / rate;
        const auto error = absolute(*frame.timestamp - expected);
        if (error > video->timeBase / Rational(2)) throw std::runtime_error("Output timestamp differs from scheduled time by more than half a container tick");
        result.maximumTimestampError = std::max(result.maximumTimestampError,error);
        ++result.decodedFrames;
        lastEnd = frame.duration ? std::optional<Rational>(*frame.timestamp + *frame.duration) : std::nullopt;
        if (progress && result.decodedFrames % 120 == 0) progress({"validating",result.decodedFrames,0,frame.timestamp,0,{}, {},{}});
    },selected,stop);
    if (scan.missingTimestamps || scan.nonIncreasingTimestamps || result.decodedFrames != expectedCount || !lastEnd)
        throw std::runtime_error("Output frame count, ordering or final-frame duration could not be verified");
    result.durationError = *lastEnd - end;
    if (absolute(result.durationError) > video->timeBase) throw std::runtime_error("Output final-frame boundary differs from the source end by more than one container tick");
    result.quantized = result.maximumTimestampError != Rational(0) || result.durationError != Rational(0) || result.auxiliaryMaximumEndError != Rational(0);
    return result;
}
std::optional<std::string> knownColorTag(const std::string& value) {
    if (value.empty() || value == "unknown" || value == "unspecified") return {};
    return value;
}
}

PipelineResult interpolateVideo(const std::string& source, const std::string& destination, const PipelineSettings& settings,
    gpu::CudaDevice& device, inference::IInterpolationBackend& backend,
    const std::function<void(const PipelineProgress&)>& progress, std::stop_token stop) {
    checkStop(stop);
    if (settings.targetRate <= Rational(0)) throw std::invalid_argument("Target frame rate must be positive");
    const auto capabilities = backend.capabilities();
    if (capabilities.modelId.empty() || !capabilities.dynamicTimestep || capabilities.spatialMultiple != 64)
        throw std::invalid_argument("Selected backend/model does not meet the dynamic RIFE contract");
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
    const auto inputProfile = media::rifeSdrInputProfile(video->color);
    const bool legacySdr8 = video->color.bitDepth == 8 &&
        std::find(capabilities.supportedInputProfiles.begin(),capabilities.supportedInputProfiles.end(),"legacy-unverified") != capabilities.supportedInputProfiles.end();
    if (!legacySdr8 && std::find(capabilities.supportedInputProfiles.begin(),capabilities.supportedInputProfiles.end(),inputProfile) == capabilities.supportedInputProfiles.end())
        throw std::invalid_argument("Selected RIFE model does not declare support for input profile " + inputProfile);
    if (!encoding.timeBase) encoding.timeBase = encodingTimeBase(video->timeBase,settings.targetRate);
    encoding.frameRate = settings.targetRate;
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
                fallback = error.what();
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
            if (progress) {
                PipelineProgress event{"encoder_fallback",0,0,{},0,fallback,encoding.encoder,{}};
                progress(event);
            }
        }
    }
    if (!writer) writer = std::make_unique<media::VideoMuxWriter>(source,initialWriterPath,encoding,stop);
    result.timings.encoderSetupSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - encoderSetupStarted).count();
    result.modelId = capabilities.modelId; result.backend = capabilities.backend;
    result.inferenceFallbackReason = capabilities.fallbackReason; result.encoderFallbackReason = fallback;
    std::optional<CachedFrame> previous;
    std::optional<CfrClock> clock;
    std::optional<media::DecodedFrame> pending;
    std::optional<Rational> pendingTime;
    std::optional<Rational> origin;
    std::shared_ptr<gpu::DeviceBuffer> reusableOutput;
    gpu::DeviceBufferPool inputBuffers(device,2);
    media::RifeSdrFrame generated{};
    const auto notify = [&](const std::string& stage, std::optional<Rational> time, const std::string& detail = {}) {
        if (progress) {
            const double seconds = elapsed();
            PipelineProgress event{stage,clock ? clock->index() : 0,result.aiFrames,time,
                seconds > 0 && clock ? static_cast<double>(clock->index()) / seconds : 0,detail,{},result.timings};
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
        if (pending) measure(result.timings.frameEncodeSeconds,[&] { writePending(*pending,*pendingTime,time - *pendingTime); });
        pending = std::move(frame); pendingTime = time;
        clock->advance();
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
    const auto pair = [&](CachedFrame& left, CachedFrame& right) {
        if (!right.source.timestamp || *right.source.timestamp <= *left.source.timestamp) throw std::runtime_error("Source timestamps must be present and strictly increasing");
        if (right.source.width != left.source.width || right.source.height != left.source.height || right.source.color.pixelFormat != left.source.color.pixelFormat ||
            right.source.color.primaries != left.source.color.primaries || right.source.color.transfer != left.source.color.transfer ||
            right.source.color.matrix != left.source.color.matrix || right.source.color.range != left.source.color.range)
            throw std::runtime_error("Midstream resolution/format/color changes require an explicit normalization policy");
        const auto analysis = measure(result.timings.sceneAnalysisSeconds,[&] {
            return quality::analyzePair(view(left.prepared),view(right.prepared),settings.scene);
        });
        if (analysis.action == quality::PairAction::sceneCutHold) ++result.sceneCuts;
        else if (analysis.action == quality::PairAction::duplicateHold) ++result.duplicatePairs;
        else if (analysis.action == quality::PairAction::abnormalMotionHold) ++result.abnormalPairs;
        if (analysis.action != quality::PairAction::interpolate) notify("quality",right.source.timestamp,std::string(quality::toString(analysis.action)));
        while (clock->timestamp() < *right.source.timestamp) {
            checkStop(stop);
            if (checkpointMode && clock->index() < completedOutputFrames) {
                const auto timestamp = clock->timestamp();
                clock->advance();
                notify("resuming",timestamp,"Reusing a verified checkpoint segment");
                continue;
            }
            if (clock->timestamp() == *left.source.timestamp) { emit(left.source); ++result.originalFrames; }
            else if (analysis.action != quality::PairAction::interpolate) { emit(left.source); ++result.heldFrames; }
            else {
                const auto time = (clock->timestamp() - *left.source.timestamp) / (*right.source.timestamp - *left.source.timestamp);
                const auto& first = upload(left); const auto& second = upload(right);
                if (!reusableOutput) reusableOutput = device.allocate(first.pixels->size(),stop);
                auto interpolated = measure(result.timings.inferenceWaitSeconds,[&] {
                    return backend.interpolate({first,second,static_cast<float>(time.toLongDouble()),std::move(reusableOutput),stop}).get();
                });
                generated.width = left.prepared.width; generated.height = left.prepared.height;
                generated.paddedWidth = left.prepared.paddedWidth; generated.paddedHeight = left.prepared.paddedHeight;
                generated.sourceColor = left.prepared.sourceColor;
                generated.pixels.resize(left.prepared.pixels.size());
                measure(result.timings.outputDownloadSeconds,[&] {
                    interpolated.frame.pixels->download(std::as_writable_bytes(std::span(generated.pixels)),0,stop);
                });
                reusableOutput = device.reclaim(std::move(interpolated.frame.pixels));
                ++result.aiFrames;
                auto restored = measure(result.timings.colorRestoreSeconds,[&] { return media::restoreRifeSdr(generated,stop); });
                emit(std::move(restored));
            }
        }
    };
    notify("decoding",{});
    double decodeCallbackSeconds = 0;
    const auto decodeStarted = std::chrono::steady_clock::now();
    const auto decoded = media::MediaEngine::decodeVideo(source,[&](const media::DecodedFrame& frame) {
        ScopedTiming callbackTiming(decodeCallbackSeconds);
        checkStop(stop);
        if (!frame.timestamp) throw std::runtime_error("Interpolation requires a known timestamp for every source frame");
        auto prepared = measure(result.timings.sourcePreparationSeconds,[&] { return media::prepareRifeSdr(frame,settings.hostFloatBufferLimit,stop); });
        CachedFrame current{frame,std::move(prepared),{}};
        if (!clock) {
            origin = frame.timestamp; clock.emplace(*origin,settings.targetRate);
            if (!segments.empty() && segments.front().contract.firstTimestamp != *origin)
                throw std::runtime_error("Checkpoint source start timestamp changed; committed segments cannot be resumed");
        }
        if (previous) pair(*previous,current);
        previous = std::move(current);
    },encoding.videoStreamIndex,stop);
    const auto decodeWallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - decodeStarted).count();
    result.timings.sourceDecodeSeconds = std::max(0.0,decodeWallSeconds - decodeCallbackSeconds);
    result.sourceFrames = decoded.frameCount;
    result.recoveredTimestamps = decoded.recoveredTimestamps;
    if (!previous) throw std::runtime_error("Source produced no video frames");
    std::optional<Rational> boundary;
    if (previous->source.duration && *previous->source.duration > Rational(0)) boundary = *previous->source.timestamp + *previous->source.duration;
    result.sourceEndEvidence = "last_frame_duration";
    if (video->declaredEnd) {
        if (*video->declaredEnd <= *previous->source.timestamp || (boundary && absolute(*video->declaredEnd - *boundary) > video->timeBase))
            throw std::runtime_error("Declared video end contradicts the final frame boundary; explicit source normalization is required");
        boundary = video->declaredEnd; result.sourceEndEvidence = "declared_stream_end";
    }
    if (!boundary) throw std::runtime_error("Final source frame and stream lack a reliable end; cannot preserve duration");
    const auto end = *boundary;
    while (clock->timestamp() < end) {
        checkStop(stop);
        if (checkpointMode && clock->index() < completedOutputFrames) {
            const auto timestamp = clock->timestamp();
            clock->advance();
            notify("resuming",timestamp,"Reusing a verified checkpoint segment");
            continue;
        }
        if (clock->timestamp() == *previous->source.timestamp) ++result.originalFrames; else ++result.heldFrames;
        emit(previous->source);
    }
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
                progress(event);
            };
            return validateOutput(temporary,encoding.videoStreamIndex,*origin,end,settings.targetRate,clock->index(),info,validationProgress,stop);
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
                assemblySeconds > 0 ? static_cast<double>(clock->index()) / assemblySeconds : 0};
            result.timings.outputFinalizeSeconds = std::max(0.0,assemblySeconds - result.timings.outputValidationSeconds);
            result.elapsedSeconds = elapsed();
            result.processingFps = result.elapsedSeconds > 0 ? static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds : 0;
            result.timings.pipelineWallSeconds = result.elapsedSeconds;
            if (settings.checkpoint->prepareOutputPublication)
                settings.checkpoint->prepareOutputPublication(temporary,result);
        };
        const auto assembly = media::MediaEngine::assembleVideoSegments(source,destination,encoding.videoStreamIndex,
            segments,stop,validateTemporary,preparePublication);
        const auto assemblySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - finishStarted).count();
        result.video = {encoding.encoder,static_cast<std::size_t>(clock->index()),assembly.videoPackets,assembly.auxiliaryPackets,
            assembly.firstTimestamp,assembly.endExclusive,assemblySeconds,
            assemblySeconds > 0 ? static_cast<double>(clock->index()) / assemblySeconds : 0};
    } else {
        result.video = writer->finish(validateTemporary);
    }
    const auto finishWallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - finishStarted).count();
    result.timings.outputFinalizeSeconds = std::max(0.0,finishWallSeconds - result.timings.outputValidationSeconds);
    result.sourceFrames = decoded.frameCount;
    result.recoveredTimestamps = decoded.recoveredTimestamps;
    result.elapsedSeconds = elapsed();
    result.processingFps = static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds;
    result.timings.pipelineWallSeconds = result.elapsedSeconds;
    notify("completed",end);
    return result;
}
} // namespace rifelove

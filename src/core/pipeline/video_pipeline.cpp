#include "core/pipeline/video_pipeline.hpp"
#include "media/rife_color.hpp"
#include "timeline/timeline.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>

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
OutputTimingValidation validateOutput(const std::string& path, int selected, Rational origin, Rational end, Rational rate,
    std::size_t expectedCount, const media::MediaInfo& source, const std::function<void(const PipelineProgress&)>& progress, std::stop_token stop) {
    const auto info = media::MediaEngine::probe(path,stop);
    const auto video = std::find_if(info.streams.begin(),info.streams.end(),[&](const media::StreamInfo& stream) { return stream.index == selected && stream.type == "video"; });
    if (video == info.streams.end() || video->timeBase <= Rational(0)) throw std::runtime_error("Output has no valid video time base");
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
        if (progress && result.decodedFrames % 120 == 0) progress({"validating",result.decodedFrames,0,frame.timestamp,0,{}});
    },selected,stop);
    if (scan.missingTimestamps || scan.nonIncreasingTimestamps || result.decodedFrames != expectedCount || !lastEnd)
        throw std::runtime_error("Output frame count, ordering or final-frame duration could not be verified");
    result.durationError = *lastEnd - end;
    if (absolute(result.durationError) > video->timeBase) throw std::runtime_error("Output final-frame boundary differs from the source end by more than one container tick");
    result.quantized = result.maximumTimestampError != Rational(0) || result.durationError != Rational(0) || result.auxiliaryMaximumEndError != Rational(0);
    return result;
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
    const auto info = media::MediaEngine::probe(source,stop);
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
    if (video->color.hdrTransfer || video->color.bitDepth > 8)
        throw std::invalid_argument("This RIFE adapter is qualified for 8-bit SDR; HDR/high-depth needs a separately validated model/color path");
    if (!encoding.timeBase) encoding.timeBase = encodingTimeBase(video->timeBase,settings.targetRate);
    encoding.frameRate = settings.targetRate;
    std::string fallback;
    std::unique_ptr<media::VideoMuxWriter> writer;
    if (encoding.encoder == "auto") {
        encoding.encoder = "h264_nvenc";
        if (media::MediaEngine::hasEncoder(encoding.encoder)) {
            try { writer = std::make_unique<media::VideoMuxWriter>(source,destination,encoding,stop); }
            catch (const media::MediaError& error) {
                if (error.operation() != "open video encoder h264_nvenc" || stop.stop_requested()) throw;
                fallback = error.what();
            }
        } else fallback = "h264_nvenc is not registered in this FFmpeg build";
        if (!writer) {
            encoding.encoder = "libopenh264";
            if (progress) progress({"encoder_fallback",0,0,{},0,fallback});
        }
    }
    if (!writer) writer = std::make_unique<media::VideoMuxWriter>(source,destination,encoding,stop);
    PipelineResult result{};
    result.modelId = capabilities.modelId; result.backend = capabilities.backend;
    result.inferenceFallbackReason = capabilities.fallbackReason; result.encoderFallbackReason = fallback;
    std::optional<CachedFrame> previous;
    std::optional<CfrClock> clock;
    std::optional<media::DecodedFrame> pending;
    std::optional<Rational> pendingTime;
    std::optional<Rational> origin;
    std::shared_ptr<gpu::DeviceBuffer> reusableOutput;
    media::RifeSdrFrame generated{};
    const auto notify = [&](const std::string& stage, std::optional<Rational> time, const std::string& detail = {}) {
        if (progress) { const double seconds = elapsed(); progress({stage,clock ? clock->index() : 0,result.aiFrames,time,seconds > 0 && clock ? static_cast<double>(clock->index()) / seconds : 0,detail}); }
    };
    const auto emit = [&](media::DecodedFrame frame) {
        const auto time = clock->timestamp();
        if (pending) writer->writeFrame(*pending,*pendingTime,time - *pendingTime);
        pending = std::move(frame); pendingTime = time;
        clock->advance();
        notify("processing",time);
    };
    const auto upload = [&](CachedFrame& frame) -> const inference::GpuFrame& {
        if (!frame.gpu) {
            auto storage = device.allocate(frame.prepared.pixels.size() * sizeof(float),stop);
            storage->upload(std::as_bytes(std::span(frame.prepared.pixels)),0,stop);
            frame.gpu = inference::GpuFrame{std::move(storage),frame.prepared.paddedWidth,frame.prepared.paddedHeight};
        }
        return *frame.gpu;
    };
    const auto pair = [&](CachedFrame& left, CachedFrame& right) {
        if (!right.source.timestamp || *right.source.timestamp <= *left.source.timestamp) throw std::runtime_error("Source timestamps must be present and strictly increasing");
        if (right.source.width != left.source.width || right.source.height != left.source.height || right.source.color.pixelFormat != left.source.color.pixelFormat ||
            right.source.color.primaries != left.source.color.primaries || right.source.color.transfer != left.source.color.transfer ||
            right.source.color.matrix != left.source.color.matrix || right.source.color.range != left.source.color.range)
            throw std::runtime_error("Midstream resolution/format/color changes require an explicit normalization policy");
        const auto analysis = quality::analyzePair(view(left.prepared),view(right.prepared),settings.scene);
        if (analysis.action == quality::PairAction::sceneCutHold) ++result.sceneCuts;
        else if (analysis.action == quality::PairAction::duplicateHold) ++result.duplicatePairs;
        else if (analysis.action == quality::PairAction::abnormalMotionHold) ++result.abnormalPairs;
        if (analysis.action != quality::PairAction::interpolate) notify("quality",right.source.timestamp,std::string(quality::toString(analysis.action)));
        while (clock->timestamp() < *right.source.timestamp) {
            checkStop(stop);
            if (clock->timestamp() == *left.source.timestamp) { emit(left.source); ++result.originalFrames; }
            else if (analysis.action != quality::PairAction::interpolate) { emit(left.source); ++result.heldFrames; }
            else {
                const auto time = (clock->timestamp() - *left.source.timestamp) / (*right.source.timestamp - *left.source.timestamp);
                const auto& first = upload(left); const auto& second = upload(right);
                if (!reusableOutput) reusableOutput = device.allocate(first.pixels->size(),stop);
                auto interpolated = backend.interpolate({first,second,static_cast<float>(time.toLongDouble()),std::move(reusableOutput),stop}).get();
                generated.width = left.prepared.width; generated.height = left.prepared.height;
                generated.paddedWidth = left.prepared.paddedWidth; generated.paddedHeight = left.prepared.paddedHeight;
                generated.sourceColor = left.prepared.sourceColor;
                generated.pixels.resize(left.prepared.pixels.size());
                interpolated.frame.pixels->download(std::as_writable_bytes(std::span(generated.pixels)),0,stop);
                reusableOutput = device.reclaim(std::move(interpolated.frame.pixels));
                ++result.aiFrames; emit(media::restoreRifeSdr(generated,stop));
            }
        }
    };
    notify("decoding",{});
    const auto decoded = media::MediaEngine::decodeVideo(source,[&](const media::DecodedFrame& frame) {
        checkStop(stop);
        if (!frame.timestamp) throw std::runtime_error("Interpolation requires a known timestamp for every source frame");
        CachedFrame current{frame,media::prepareRifeSdr(frame,settings.hostFloatBufferLimit,stop),{}};
        if (!clock) { origin = frame.timestamp; clock.emplace(*origin,settings.targetRate); }
        if (previous) pair(*previous,current);
        previous = std::move(current);
    },encoding.videoStreamIndex,stop);
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
        if (clock->timestamp() == *previous->source.timestamp) ++result.originalFrames; else ++result.heldFrames;
        emit(previous->source);
    }
    if (!pending || !pendingTime || end <= *pendingTime) throw std::runtime_error("No valid final output frame boundary");
    writer->writeFrame(*pending,*pendingTime,end - *pendingTime);
    notify("validating",*pendingTime);
    result.video = writer->finish([&](const std::string& temporary) {
        result.validation = validateOutput(temporary,encoding.videoStreamIndex,*origin,end,settings.targetRate,clock->index(),info,progress,stop);
    });
    result.sourceFrames = decoded.frameCount;
    result.elapsedSeconds = elapsed();
    result.processingFps = static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds;
    notify("completed",end);
    return result;
}
} // namespace rifelove

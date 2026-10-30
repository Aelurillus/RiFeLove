#include "media/output_helpers.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>

namespace rifelove::media {
namespace {

Rational absolute(Rational value) {
    return value < Rational(0) ? Rational(0) - value : value;
}

bool withinTick(Rational actual, Rational expected, Rational timeBase) {
    return absolute(actual - expected) <= timeBase;
}

AVRational exactAssemblyTimeBase(const std::vector<VideoSegmentInput>& segments) {
    std::int64_t denominator = 1;
    const auto include = [&](Rational value) {
        const auto factor = value.denominator() / std::gcd(denominator,value.denominator());
        if (factor > std::numeric_limits<int>::max() / denominator)
            throw std::runtime_error("checkpoint timeline cannot be represented by a 32-bit container time base");
        denominator *= factor;
    };
    for (const auto& segment : segments) {
        include(segment.contract.firstTimestamp);
        include(segment.contract.frameDuration);
        if (segment.contract.endExclusive) include(*segment.contract.endExclusive);
    }
    return {1,static_cast<int>(denominator)};
}

std::int64_t exactTicks(Rational value, AVRational timeBase) {
    const auto scaled = value / detail::rational(timeBase);
    if (scaled.denominator() != 1)
        throw std::runtime_error("output container time base cannot exactly represent the checkpoint timeline");
    return scaled.numerator();
}

// Matroska checkpoints use millisecond timestamps. Once a segment has passed
// decoded-frame verification, its contract is the exact CFR clock; rescaling
// the rounded MKV timestamps into MP4's finer clock would retain that rounding
// and shorten 30 frames at 60 FPS to 499 ms. Recover only timestamps that can
// be unambiguously identified on the verified contract's frame grid.
Rational contractTimestamp(std::int64_t timestamp, AVRational sourceTimeBase,
    const VideoSegmentContract& contract, bool decodeTimestamp = false) {
    const auto actual = Rational(timestamp) * detail::rational(sourceTimeBase);
    const auto frameOffset = (actual - contract.firstTimestamp) / contract.frameDuration;
    const auto nearest = av_rescale_rnd(frameOffset.numerator(),1,frameOffset.denominator(),AV_ROUND_NEAR_INF);
    const auto expected = contract.firstTimestamp + Rational(nearest) * contract.frameDuration;
    // Matroska may infer the initial DTS from a rounded PTS minus a truncated
    // frame duration, adding at most one more container tick of uncertainty.
    // A reordered DTS is recoverable only while that interval identifies a
    // single point on the contract clock.
    const auto tolerance = detail::rational(sourceTimeBase) * Rational(decodeTimestamp ? 3 : 1,2);
    if (detail::rational(sourceTimeBase) > contract.frameDuration ||
        (decodeTimestamp && tolerance * Rational(2) >= contract.frameDuration) ||
        absolute(actual - expected) > tolerance)
        throw std::runtime_error("checkpoint packet timestamp cannot be uniquely restored to its verified frame timeline");
    return expected;
}

void restorePacketTimeline(AVPacket* packet, AVRational sourceTimeBase, AVRational destinationTimeBase,
    const VideoSegmentContract& contract, std::vector<bool>& seen) {
    if (packet->pts == AV_NOPTS_VALUE)
        throw std::runtime_error("checkpoint packet has no presentation timestamp for exact timeline assembly");
    const auto presentation = contractTimestamp(packet->pts,sourceTimeBase,contract);
    const auto frameIndex = (presentation - contract.firstTimestamp) / contract.frameDuration;
    if (frameIndex.numerator() < 0 || static_cast<std::uint64_t>(frameIndex.numerator()) >= contract.frameCount ||
        seen[static_cast<std::size_t>(frameIndex.numerator())])
        throw std::runtime_error("checkpoint packet presentation timestamps do not uniquely cover its verified frames");
    const auto index = static_cast<std::size_t>(frameIndex.numerator());
    seen[index] = true;
    const auto nominalEnd = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(contract.frameCount));
    const auto duration = index + 1 == contract.frameCount ? contract.endExclusive.value_or(nominalEnd) - presentation : contract.frameDuration;
    if (packet->dts != AV_NOPTS_VALUE)
        packet->dts = exactTicks(packet->dts == packet->pts ? presentation :
            contractTimestamp(packet->dts,sourceTimeBase,contract,true),destinationTimeBase);
    packet->pts = exactTicks(presentation,destinationTimeBase);
    packet->duration = exactTicks(duration,destinationTimeBase);
}

void requireOptionalMatch(const std::optional<std::string>& expected, const std::string& actual, const char* field) {
    if (expected && *expected != actual) throw std::runtime_error(std::string("segment ") + field + " does not match its checkpoint contract");
}

bool sameVideoParameters(const AVCodecParameters* left, const AVCodecParameters* right) {
    if (left->codec_type != right->codec_type || left->codec_id != right->codec_id || left->profile != right->profile ||
        left->level != right->level || left->format != right->format || left->width != right->width || left->height != right->height ||
        left->field_order != right->field_order || left->color_range != right->color_range ||
        left->color_primaries != right->color_primaries || left->color_trc != right->color_trc ||
        left->color_space != right->color_space || left->chroma_location != right->chroma_location ||
        left->sample_aspect_ratio.num != right->sample_aspect_ratio.num ||
        left->sample_aspect_ratio.den != right->sample_aspect_ratio.den ||
        left->bits_per_coded_sample != right->bits_per_coded_sample || left->bits_per_raw_sample != right->bits_per_raw_sample ||
        left->extradata_size != right->extradata_size || left->nb_coded_side_data != right->nb_coded_side_data)
        return false;
    if (left->extradata_size > 0 && (!left->extradata || !right->extradata ||
        std::memcmp(left->extradata,right->extradata,static_cast<std::size_t>(left->extradata_size)) != 0))
        return false;
    for (int i = 0; i < left->nb_coded_side_data; ++i) {
        const auto& a = left->coded_side_data[i];
        const auto& b = right->coded_side_data[i];
        if (a.type != b.type || a.size != b.size || (a.size > 0 &&
            (!a.data || !b.data || std::memcmp(a.data,b.data,a.size) != 0)))
            return false;
    }
    return true;
}

class AuxiliaryPacketCursor final {
public:
    AuxiliaryPacketCursor(AVFormatContext* source, AVFormatContext* output, int selectedVideo,
        std::stop_token stop)
        : source_(source), output_(output), selectedVideo_(selectedVideo), stop_(stop),
          packet_(av_packet_alloc(),[](AVPacket* packet) { av_packet_free(&packet); }) {
        if (!packet_) throw std::bad_alloc();
    }

    bool advance() {
        detail::checkStopped(stop_);
        av_packet_unref(packet_.get());
        for (;;) {
            const int status = av_read_frame(source_,packet_.get());
            if (status == AVERROR_EOF) { available_ = false; return false; }
            detail::check(status,"read original auxiliary packet");
            const int sourceIndex = packet_->stream_index;
            if (sourceIndex == selectedVideo_) { av_packet_unref(packet_.get()); continue; }
            if (sourceIndex < 0 || static_cast<unsigned>(sourceIndex) >= source_->nb_streams ||
                static_cast<unsigned>(sourceIndex) >= output_->nb_streams)
                throw std::runtime_error("Source stream layout changed during segment assembly");
            const auto* incoming = source_->streams[sourceIndex];
            const auto* outgoing = output_->streams[sourceIndex];
            detail::normalizeCopiedAudioDuration(packet_.get(),incoming,detail::declaredEnd(source_,incoming));
            av_packet_rescale_ts(packet_.get(),incoming->time_base,outgoing->time_base);
            packet_->stream_index = sourceIndex;
            packet_->pos = -1;
            const auto timestamp = packet_->dts != AV_NOPTS_VALUE ? packet_->dts : packet_->pts;
            if (timestamp == AV_NOPTS_VALUE) throw std::runtime_error("Original auxiliary packet has no presentation or decode timestamp");
            available_ = true;
            return true;
        }
    }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] Rational timestamp() const {
        if (!available_) throw std::logic_error("no original auxiliary packet is ready");
        const auto* stream = output_->streams[packet_->stream_index];
        const auto value = packet_->dts != AV_NOPTS_VALUE ? packet_->dts : packet_->pts;
        return Rational(value) * detail::rational(stream->time_base);
    }

    void write() {
        if (!available_) throw std::logic_error("no original auxiliary packet is ready");
        detail::check(av_interleaved_write_frame(output_,packet_.get()),"write original auxiliary packet");
        ++written_;
        av_packet_unref(packet_.get());
        available_ = false;
    }

    [[nodiscard]] std::size_t written() const noexcept { return written_; }

private:
    AVFormatContext* source_;
    AVFormatContext* output_;
    int selectedVideo_;
    std::stop_token stop_;
    std::unique_ptr<AVPacket,void(*)(AVPacket*)> packet_;
    bool available_ = false;
    std::size_t written_ = 0;
};

bool readVideoPacket(AVFormatContext* input, AVPacket* packet, std::stop_token stop) {
    for (;;) {
        detail::checkStopped(stop);
        const int status = av_read_frame(input,packet);
        if (status == AVERROR_EOF) return false;
        detail::check(status,"read checkpoint video packet");
        if (packet->stream_index != 0)
            throw std::runtime_error("checkpoint segment contains an unexpected non-video packet");
        return true;
    }
}

} // namespace

VideoSegmentAssemblyResult MediaEngine::assembleVideoSegments(const std::string& source, const std::string& destination,
    int videoStreamIndex, const std::vector<VideoSegmentInput>& segments, std::stop_token stop,
    const std::function<void(const std::string&)>& validate,
    const std::function<void(const std::string&,const VideoSegmentAssemblyResult&)>& beforePublish) {
    detail::checkStopped(stop);
    if (segments.empty()) throw std::invalid_argument("at least one verified video segment is required");
    const auto sourceInfo = probe(source,stop);
    if (videoStreamIndex < 0) videoStreamIndex = sourceInfo.preferredVideoStream;
    if (videoStreamIndex < 0 || static_cast<std::size_t>(videoStreamIndex) >= sourceInfo.streams.size() ||
        sourceInfo.streams[static_cast<std::size_t>(videoStreamIndex)].index != videoStreamIndex ||
        sourceInfo.streams[static_cast<std::size_t>(videoStreamIndex)].type != "video")
        throw std::invalid_argument("segment assembly requires a valid selected source video stream");

    std::optional<Rational> previousEnd;
    Rational firstTimestamp;
    Rational expectedEnd;
    std::size_t expectedFrameCount = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        detail::checkStopped(stop);
        const auto& item = segments[i];
        if (previousEnd && item.contract.firstTimestamp != *previousEnd)
            throw std::invalid_argument("video segment contracts must be contiguous on the global timeline");
        if (item.contract.frameCount > std::numeric_limits<std::size_t>::max() - expectedFrameCount)
            throw std::invalid_argument("assembled video frame count exceeds the supported range");
        const auto segmentResult = verifyVideoSegment(item.path,item.contract,stop);
        const auto end = segmentResult.endExclusive;
        if (i == 0) firstTimestamp = item.contract.firstTimestamp;
        previousEnd = end;
        expectedEnd = end;
        expectedFrameCount += item.contract.frameCount;
    }

    const auto destinationPath = detail::pathFromUtf8(destination);
    const auto extension = detail::outputExtension(destinationPath);
    const bool restoreExactTimeline = extension == ".mp4" || extension == ".mov";
    const auto assemblyTimeBase = restoreExactTimeline ? exactAssemblyTimeBase(segments) : AVRational{0,1};
    detail::Input sourceInput(source,stop);
    if (static_cast<unsigned>(videoStreamIndex) >= sourceInput.context->nb_streams ||
        sourceInput.context->streams[videoStreamIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::invalid_argument("selected source video stream changed before segment assembly");
    auto firstSegment = std::make_unique<detail::Input>(segments.front().path,stop);
    if (firstSegment->context->nb_streams != 1 || firstSegment->context->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::runtime_error("checkpoint segment must contain exactly one video stream");
    const auto* firstSegmentStream = firstSegment->context->streams[0];
    const std::string formatName = extension == ".mkv" ? "matroska" : extension == ".mov" ? "mov" : "mp4";
    detail::TemporaryOutput temporary(destinationPath);
    detail::OutputContext output;
    const auto temporaryPath = detail::pathToUtf8(temporary.path);
    detail::check(avformat_alloc_output_context2(&output.value,nullptr,formatName.c_str(),temporaryPath.c_str()),"select segment assembly container");
    if (!output.value) throw std::bad_alloc();
    output.value->interrupt_callback = sourceInput.context->interrupt_callback;
    output.value->max_interleave_delta = AV_TIME_BASE;
    detail::check(av_dict_copy(&output.value->metadata,sourceInput.context->metadata,0),"copy segment assembly container metadata");
    detail::copyChapters(sourceInput.context,output.value);

    for (unsigned i = 0; i < sourceInput.context->nb_streams; ++i) {
        detail::checkStopped(stop);
        const auto* sourceStream = sourceInput.context->streams[i];
        const auto* parameters = static_cast<int>(i) == videoStreamIndex ? firstSegmentStream->codecpar : sourceStream->codecpar;
        const auto* metadata = static_cast<int>(i) == videoStreamIndex ? firstSegmentStream->metadata : sourceStream->metadata;
        if (avformat_query_codec(output.value->oformat,parameters->codec_id,FF_COMPLIANCE_NORMAL) == 0)
            throw std::runtime_error("output container does not support stream " + std::to_string(i) + " (" + avcodec_get_name(parameters->codec_id) + ")");
        auto* outgoing = avformat_new_stream(output.value,nullptr);
        if (!outgoing) throw std::bad_alloc();
        detail::check(avcodec_parameters_copy(outgoing->codecpar,parameters),"copy segment assembly stream parameters");
        outgoing->codecpar->codec_tag = 0;
        outgoing->time_base = static_cast<int>(i) == videoStreamIndex ?
            (restoreExactTimeline ? assemblyTimeBase : firstSegmentStream->time_base) : sourceStream->time_base;
        outgoing->avg_frame_rate = static_cast<int>(i) == videoStreamIndex ? firstSegmentStream->avg_frame_rate : sourceStream->avg_frame_rate;
        outgoing->sample_aspect_ratio = static_cast<int>(i) == videoStreamIndex ? firstSegmentStream->sample_aspect_ratio : sourceStream->sample_aspect_ratio;
        outgoing->disposition = sourceStream->disposition;
        detail::copyStreamMetadata(&outgoing->metadata,metadata,extension);
    }
    if (extension == ".mkv") {
        const auto padding = detail::initialAudioPadding(source,stop);
        if (padding.size() != output.value->nb_streams) throw std::runtime_error("Input stream layout changed while inspecting audio priming");
        for (unsigned i = 0; i < output.value->nb_streams; ++i)
            if (output.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                output.value->streams[i]->codecpar->initial_padding = padding[i];
    }
    detail::writeOutputHeader(output.value,temporaryPath,extension);

    AuxiliaryPacketCursor auxiliary(sourceInput.context,output.value,videoStreamIndex,stop);
    (void)auxiliary.advance();
    const auto* outputVideo = output.value->streams[videoStreamIndex];
    std::unique_ptr<AVPacket,void(*)(AVPacket*)> packet(av_packet_alloc(),[](AVPacket* value) { av_packet_free(&value); });
    if (!packet) throw std::bad_alloc();
    std::optional<Rational> previousDts;
    std::size_t videoPackets = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        detail::checkStopped(stop);
        std::unique_ptr<detail::Input> ownedSegment;
        AVFormatContext* segmentContext = nullptr;
        if (i == 0) segmentContext = firstSegment->context;
        else { ownedSegment = std::make_unique<detail::Input>(segments[i].path,stop); segmentContext = ownedSegment->context; }
        if (!segmentContext || segmentContext->nb_streams != 1 || segmentContext->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            throw std::runtime_error("checkpoint segment stream layout changed during assembly");
        if (!sameVideoParameters(firstSegmentStream->codecpar,segmentContext->streams[0]->codecpar))
            throw std::runtime_error("checkpoint segment codec parameters or extradata differ; stream-copy assembly is unsafe");
        std::vector<bool> seen(restoreExactTimeline ? segments[i].contract.frameCount : 0,false);
        bool finalPacketFound = false;
        for (;;) {
            av_packet_unref(packet.get());
            if (!readVideoPacket(segmentContext,packet.get(),stop)) break;
            if (restoreExactTimeline)
                restorePacketTimeline(packet.get(),segmentContext->streams[0]->time_base,outputVideo->time_base,segments[i].contract,seen);
            else {
                av_packet_rescale_ts(packet.get(),segmentContext->streams[0]->time_base,outputVideo->time_base);
                // Matroska's demuxer may truncate an inferred packet duration
                // independently of its rounded PTS. Restore the final packet's
                // duration to the nearest representable contract boundary so
                // the muxed track does not silently lose a millisecond.
                if (packet->pts != AV_NOPTS_VALUE) {
                    const auto& contract = segments[i].contract;
                    const auto lastStart = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(contract.frameCount - 1));
                    const auto tick = detail::rational(outputVideo->time_base);
                    const auto presentation = Rational(packet->pts) * tick;
                    if (absolute(presentation - lastStart) <= tick / Rational(2)) {
                        if (finalPacketFound) throw std::runtime_error("checkpoint contains ambiguous final packet timestamps");
                        const auto nominalEnd = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(contract.frameCount));
                        const auto endTicks = contract.endExclusive.value_or(nominalEnd) / tick;
                        const auto roundedEnd = av_rescale_rnd(endTicks.numerator(),1,endTicks.denominator(),AV_ROUND_NEAR_INF);
                        if (roundedEnd <= packet->pts) throw std::runtime_error("checkpoint final frame cannot be represented in the output time base");
                        packet->duration = roundedEnd - packet->pts;
                        finalPacketFound = true;
                    }
                }
            }
            packet->stream_index = videoStreamIndex;
            packet->pos = -1;
            const auto videoTimestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
            if (videoTimestamp == AV_NOPTS_VALUE) throw std::runtime_error("checkpoint video packet has no presentation or decode timestamp");
            const auto videoTime = Rational(videoTimestamp) * detail::rational(outputVideo->time_base);
            if (previousDts && videoTime < *previousDts) throw std::runtime_error("checkpoint video decode timestamps are not monotonic across segments");
            previousDts = videoTime;
            while (auxiliary.available() && auxiliary.timestamp() <= videoTime) {
                auxiliary.write();
                (void)auxiliary.advance();
            }
            detail::check(av_interleaved_write_frame(output.value,packet.get()),"write checkpoint video packet");
            ++videoPackets;
            av_packet_unref(packet.get());
        }
        if (restoreExactTimeline && std::find(seen.begin(),seen.end(),false) != seen.end())
            throw std::runtime_error("checkpoint packets do not cover every verified frame");
        if (!restoreExactTimeline && !finalPacketFound)
            throw std::runtime_error("checkpoint final frame packet cannot be identified for output");
    }
    while (auxiliary.available()) {
        auxiliary.write();
        (void)auxiliary.advance();
    }
    detail::check(av_write_trailer(output.value),"finalize assembled video");
    detail::check(avio_closep(&output.value->pb),"close assembled video");
    detail::checkStopped(stop);

    const auto verified = probe(temporaryPath,stop);
    if (verified.streams.size() != sourceInfo.streams.size()) throw std::runtime_error("assembled output stream count differs from the source layout");
    const auto& outputVideoInfo = verified.streams[static_cast<std::size_t>(videoStreamIndex)];
    const auto& firstContract = segments.front().contract;
    if (outputVideoInfo.codec != firstContract.codec || outputVideoInfo.width != firstContract.width || outputVideoInfo.height != firstContract.height ||
        outputVideoInfo.color.pixelFormat != firstContract.pixelFormat || outputVideoInfo.color.bitDepth != firstContract.bitDepth)
        throw std::runtime_error("assembled video codec, dimensions, or pixel format changed");
    if (outputVideoInfo.declaredEnd && !withinTick(*outputVideoInfo.declaredEnd,expectedEnd,outputVideoInfo.timeBase))
        throw std::runtime_error("assembled video declared end differs from the checkpoint timeline");
    for (std::size_t i = 0; i < sourceInfo.streams.size(); ++i) {
        if (static_cast<int>(i) == videoStreamIndex) continue;
        const auto& original = sourceInfo.streams[i];
        const auto& copied = verified.streams[i];
        if (original.index != copied.index || original.type != copied.type || original.codec != copied.codec ||
            original.sampleRate != copied.sampleRate || original.channels != copied.channels)
            throw std::runtime_error("assembled output did not preserve an original auxiliary stream");
        if (original.declaredEnd) {
            if (!copied.declaredEnd || absolute(*copied.declaredEnd - *original.declaredEnd) > std::max(original.timeBase,copied.timeBase))
                throw std::runtime_error("assembled output changed an auxiliary stream presentation end");
        }
    }
    if (verified.chapters.size() < sourceInfo.chapters.size()) throw std::runtime_error("assembled output did not preserve all source chapters");
    for (const auto& segment : segments) {
        requireOptionalMatch(segment.contract.primaries,outputVideoInfo.color.primaries,"assembled color primaries");
        requireOptionalMatch(segment.contract.transfer,outputVideoInfo.color.transfer,"assembled color transfer");
        requireOptionalMatch(segment.contract.matrix,outputVideoInfo.color.matrix,"assembled color matrix");
        requireOptionalMatch(segment.contract.range,outputVideoInfo.color.range,"assembled color range");
    }

    std::size_t currentSegment = 0;
    std::size_t frameInSegment = 0;
    std::size_t decodedFrames = 0;
    std::optional<Rational> finalFrameDuration;
    const auto decoded = decodeVideo(temporaryPath,[&](const DecodedFrame& frame) {
        if (!frame.timestamp || frame.timestampRecovered || currentSegment >= segments.size())
            throw std::runtime_error("assembled video has missing timestamps or unexpected frames");
        const auto& contract = segments[currentSegment].contract;
        const auto expected = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(frameInSegment));
        if (absolute(*frame.timestamp - expected) > outputVideoInfo.timeBase / Rational(2))
            throw std::runtime_error("assembled frame timestamp differs from its global segment timeline");
        ++frameInSegment;
        ++decodedFrames;
        if (frame.duration && *frame.duration > Rational(0)) finalFrameDuration = frame.duration;
        else finalFrameDuration.reset();
        if (frameInSegment == contract.frameCount) { ++currentSegment; frameInSegment = 0; }
    },videoStreamIndex,stop);
    if (decoded.missingTimestamps || decoded.nonIncreasingTimestamps || decodedFrames != expectedFrameCount || currentSegment != segments.size())
        throw std::runtime_error("assembled output frame count or timestamp ordering differs from its checkpoint segments");
    const auto& finalContract = segments.back().contract;
    const auto finalStart = finalContract.firstTimestamp + finalContract.frameDuration * Rational(static_cast<std::int64_t>(finalContract.frameCount - 1));
    if (finalFrameDuration && absolute(*finalFrameDuration - (expectedEnd - finalStart)) > outputVideoInfo.timeBase)
        throw std::runtime_error("assembled output final frame boundary differs from its checkpoint timeline");
    if (!finalFrameDuration && (!outputVideoInfo.declaredEnd || absolute(*outputVideoInfo.declaredEnd - expectedEnd) > outputVideoInfo.timeBase))
        throw std::runtime_error("assembled output final frame boundary cannot be verified");
    if (validate) validate(temporaryPath);
    const VideoSegmentAssemblyResult result{segments.size(),videoPackets,auxiliary.written(),firstTimestamp,expectedEnd};
    if (beforePublish) beforePublish(temporaryPath,result);
    detail::checkStopped(stop);
    temporary.publish();
    return result;
}

} // namespace rifelove::media

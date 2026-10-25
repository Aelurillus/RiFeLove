#include "media/ffmpeg_helpers.hpp"

#include <limits>
#include <memory>

namespace rifelove::media {
namespace {

Rational absolute(Rational value) {
    return value < Rational(0) ? Rational(0) - value : value;
}

bool withinTick(Rational actual, Rational expected, Rational timeBase) {
    return absolute(actual - expected) <= timeBase / Rational(2);
}

void requireOptionalMatch(const std::optional<std::string>& expected, const std::string& actual, const char* field) {
    if (expected && *expected != actual) throw std::runtime_error(std::string("segment ") + field + " does not match its checkpoint contract");
}

} // namespace

VideoSegmentVerification MediaEngine::verifyVideoSegment(const std::string& path,
    const VideoSegmentContract& contract, std::stop_token stop) {
    detail::checkStopped(stop);
    if (contract.frameCount == 0 || contract.width <= 0 || contract.height <= 0 ||
        contract.frameDuration <= Rational(0) || contract.codec.empty() || contract.pixelFormat.empty() || contract.bitDepth <= 0)
        throw std::invalid_argument("video segment contract is incomplete");
    if (contract.frameCount > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("video segment frame count exceeds the supported range");

    const auto nominalEnd = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(contract.frameCount));
    const auto lastFrameStart = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(contract.frameCount - 1));
    const auto expectedEnd = contract.endExclusive.value_or(nominalEnd);
    if (expectedEnd <= lastFrameStart || expectedEnd > nominalEnd)
        throw std::invalid_argument("video segment contract has an invalid end boundary");

    const auto info = probe(path, stop);
    if (info.streams.size() != 1 || info.preferredVideoStream < 0 || info.streams.front().type != "video")
        throw std::runtime_error("checkpoint segment must contain exactly one video stream");
    const auto& stream = info.streams.front();
    if (stream.codec != contract.codec || stream.width != contract.width || stream.height != contract.height ||
        stream.color.pixelFormat != contract.pixelFormat || stream.color.bitDepth != contract.bitDepth)
        throw std::runtime_error("segment codec, dimensions, or pixel format does not match its checkpoint contract");
    requireOptionalMatch(contract.primaries, stream.color.primaries, "color primaries");
    requireOptionalMatch(contract.transfer, stream.color.transfer, "color transfer");
    requireOptionalMatch(contract.matrix, stream.color.matrix, "color matrix");
    requireOptionalMatch(contract.range, stream.color.range, "color range");
    if (stream.timeBase <= Rational(0)) throw std::runtime_error("segment has an invalid stream time base");
    const auto tick = stream.timeBase;

    // Validate the compressed stream's first packet independently of decoder
    // behavior; the first frame of every committed segment must be seekable.
    {
        detail::Input input(path, stop);
        const auto freePacket = [](AVPacket* packet) { av_packet_free(&packet); };
        std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
        if (!packet) throw std::bad_alloc();
        bool foundVideoPacket = false;
        for (;;) {
            detail::checkStopped(stop);
            const int status = av_read_frame(input.context, packet.get());
            if (status == AVERROR_EOF) break;
            detail::check(status, "read checkpoint segment packet");
            if (packet->stream_index == stream.index) {
                foundVideoPacket = true;
                if ((packet->flags & AV_PKT_FLAG_KEY) == 0)
                    throw std::runtime_error("checkpoint segment does not begin with a keyframe packet");
                break;
            }
            av_packet_unref(packet.get());
        }
        if (!foundVideoPacket) throw std::runtime_error("checkpoint segment contains no video packets");
    }

    std::size_t decodedFrames = 0;
    std::optional<Rational> actualFirst;
    std::optional<Rational> actualEnd;
    const auto decoded = decodeVideo(path, [&](const DecodedFrame& frame) {
        if (!frame.timestamp || frame.timestampRecovered)
            throw std::runtime_error("checkpoint segment contains a missing or reconstructed frame timestamp");
        if (frame.width != contract.width || frame.height != contract.height ||
            frame.color.pixelFormat != contract.pixelFormat || frame.color.bitDepth != contract.bitDepth)
            throw std::runtime_error("decoded segment frame does not match its checkpoint video format");
        requireOptionalMatch(contract.primaries, frame.color.primaries, "decoded color primaries");
        requireOptionalMatch(contract.transfer, frame.color.transfer, "decoded color transfer");
        requireOptionalMatch(contract.matrix, frame.color.matrix, "decoded color matrix");
        requireOptionalMatch(contract.range, frame.color.range, "decoded color range");
        if (decodedFrames >= contract.frameCount)
            throw std::runtime_error("checkpoint segment contains more frames than its contract");
        const auto expectedTimestamp = contract.firstTimestamp + contract.frameDuration * Rational(static_cast<std::int64_t>(decodedFrames));
        if (!withinTick(*frame.timestamp, expectedTimestamp, tick))
            throw std::runtime_error("checkpoint segment frame timestamp does not match the global timeline");
        if (!actualFirst) actualFirst = frame.timestamp;
        if (frame.duration && *frame.duration > Rational(0)) actualEnd = *frame.timestamp + *frame.duration;
        ++decodedFrames;
    }, stream.index, stop);

    if (decodedFrames != contract.frameCount || decoded.frameCount != contract.frameCount)
        throw std::runtime_error("checkpoint segment frame count does not match its contract");
    if (!actualFirst || !withinTick(*actualFirst, contract.firstTimestamp, tick))
        throw std::runtime_error("checkpoint segment first timestamp does not match its contract");
    if (actualEnd && absolute(*actualEnd - expectedEnd) > tick)
        throw std::runtime_error("checkpoint segment final frame duration " + actualEnd->toString() +
            " does not match its contract boundary " + expectedEnd.toString() + " (container tick " + tick.toString() + ")");
    if (stream.declaredEnd && absolute(*stream.declaredEnd - expectedEnd) > tick)
        throw std::runtime_error("checkpoint segment declared end does not match its contract");
    if (!actualEnd && !stream.declaredEnd)
        throw std::runtime_error("checkpoint segment end boundary cannot be verified from the media container");

    return {decodedFrames, *actualFirst, expectedEnd, stream.codec, stream.color.pixelFormat};
}

} // namespace rifelove::media

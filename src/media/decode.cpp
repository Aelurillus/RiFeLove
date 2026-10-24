#include "media/ffmpeg_helpers.hpp"
#include "media/rife_color.hpp"

namespace rifelove::media {

DecodeSummary MediaEngine::decodeVideo(const std::string& path,
    const std::function<void(const DecodedFrame&)>& onFrame, int streamIndex, std::stop_token stop) {
    detail::Input input(path, stop);
    if (streamIndex < 0) streamIndex = av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    detail::check(streamIndex, "find video stream");
    if (static_cast<unsigned>(streamIndex) >= input.context->nb_streams ||
        input.context->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::invalid_argument("selected stream is not a video stream");
    auto* stream = input.context->streams[streamIndex];
    const auto streamColor = detail::color(stream->codecpar);
    const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) throw std::runtime_error("no decoder is available for this video codec");
    auto freeCodec = [](AVCodecContext* p) { avcodec_free_context(&p); };
    auto freePacket = [](AVPacket* p) { av_packet_free(&p); };
    auto freeFrame = [](AVFrame* p) { av_frame_free(&p); };
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> context(avcodec_alloc_context3(decoder), freeCodec);
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    if (!context || !packet || !frame) throw std::bad_alloc();
    detail::check(avcodec_parameters_to_context(context.get(), stream->codecpar), "configure video decoder");
    context->pkt_timebase = stream->time_base;
    detail::check(avcodec_open2(context.get(), decoder, nullptr), "open video decoder");

    DecodeSummary summary;
    std::optional<Rational> previous;
    std::optional<Rational> delta;
    bool variable = false;
    const auto rate = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    const bool hasRate = rate.num > 0 && rate.den > 0;
    bool matchesQuantizedConstantRate = hasRate;
    auto receive = [&]() {
        for (;;) {
            detail::checkStopped(stop);
            const int status = avcodec_receive_frame(context.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
            detail::check(status, "receive decoded frame");
            const auto pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
            const auto timestamp = detail::timestamp(pts, stream->time_base);
            if (!timestamp) ++summary.missingTimestamps;
            else {
                if (!summary.firstTimestamp) summary.firstTimestamp = timestamp;
                summary.lastTimestamp = timestamp;
                if (hasRate) {
                    const auto expected = *summary.firstTimestamp + Rational(static_cast<std::int64_t>(summary.frameCount)) / detail::rational(rate);
                    auto error = *timestamp - expected;
                    if (error < Rational(0)) error = Rational(0) - error;
                    if (error > detail::rational(stream->time_base)) matchesQuantizedConstantRate = false;
                }
                if (previous) {
                    const auto step = *timestamp - *previous;
                    if (step <= Rational(0)) ++summary.nonIncreasingTimestamps;
                    else if (!delta) delta = step;
                    else if (step != *delta) variable = true;
                }
                previous = timestamp;
            }
            if (onFrame) {
                std::shared_ptr<AVFrame> retained(av_frame_clone(frame.get()), freeFrame);
                if (!retained) throw std::bad_alloc();
                DecodedFrame output{};
                output.index = summary.frameCount;
                output.width = retained->width;
                output.height = retained->height;
                output.timestamp = timestamp;
                if (retained->duration > 0) output.duration = detail::timestamp(retained->duration, stream->time_base);
                output.color = normalizeDecodedSdrColor(detail::color(retained.get()), streamColor);
                for (std::size_t i = 0; i < output.planes.size(); ++i) output.planes[i] = {retained->data[i], retained->linesize[i]};
                output.storage = std::move(retained);
                onFrame(output);
            }
            ++summary.frameCount;
            av_frame_unref(frame.get());
        }
    };

    for (;;) {
        detail::checkStopped(stop);
        const int read = av_read_frame(input.context, packet.get());
        if (read == AVERROR_EOF) break;
        detail::check(read, "read media packet");
        if (packet->stream_index == streamIndex) {
            int sent = avcodec_send_packet(context.get(), packet.get());
            if (sent == AVERROR(EAGAIN)) { receive(); sent = avcodec_send_packet(context.get(), packet.get()); }
            detail::check(sent, "send video packet to decoder");
            receive();
        }
        av_packet_unref(packet.get());
    }
    detail::check(avcodec_send_packet(context.get(), nullptr), "flush video decoder");
    if (receive() != AVERROR_EOF) throw std::runtime_error("decoder did not reach EOF after flushing");
    if (summary.frameCount == 0) throw std::runtime_error("video decoder produced no frames");
    if (summary.missingTimestamps || summary.nonIncreasingTimestamps) summary.cadence = "invalid_timestamps";
    else if (delta) summary.cadence = variable && !matchesQuantizedConstantRate ? "variable" : "constant";
    return summary;
}

} // namespace rifelove::media

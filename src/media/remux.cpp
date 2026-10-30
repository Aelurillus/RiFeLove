#include "media/output_helpers.hpp"

namespace rifelove::media {

RemuxResult MediaEngine::remux(const std::string& source, const std::string& destination, std::stop_token stop) {
    detail::checkStopped(stop);
    const auto destinationPath = detail::pathFromUtf8(destination);
    const auto extension = detail::outputExtension(destinationPath);
    detail::Input input(source, stop);
    detail::TemporaryOutput temporary(destinationPath);
    detail::OutputContext output;
    detail::check(avformat_alloc_output_context2(&output.value, nullptr, nullptr, destination.c_str()), "select output container");
    if (!output.value) throw std::bad_alloc();
    output.value->interrupt_callback = input.context->interrupt_callback;
    detail::check(av_dict_copy(&output.value->metadata, input.context->metadata, 0), "copy container metadata");
    detail::copyChapters(input.context, output.value);
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        auto* incoming = input.context->streams[i];
        if (avformat_query_codec(output.value->oformat, incoming->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0)
            throw std::runtime_error("output container does not support stream " + std::to_string(i) + " (" + avcodec_get_name(incoming->codecpar->codec_id) + "); choose a compatible container");
        auto* outgoing = avformat_new_stream(output.value, nullptr);
        if (!outgoing) throw std::bad_alloc();
        detail::check(avcodec_parameters_copy(outgoing->codecpar, incoming->codecpar), "copy stream parameters");
        outgoing->codecpar->codec_tag = 0;
        outgoing->time_base = incoming->time_base;
        outgoing->avg_frame_rate = incoming->avg_frame_rate;
        outgoing->sample_aspect_ratio = incoming->sample_aspect_ratio;
        outgoing->disposition = incoming->disposition;
        detail::copyStreamMetadata(&outgoing->metadata, incoming->metadata, extension);
    }
    const auto temporaryPath = detail::pathToUtf8(temporary.path);
    if (extension == ".mkv") {
        const auto padding = detail::initialAudioPadding(source,stop);
        if (padding.size() != output.value->nb_streams) throw std::runtime_error("Input stream layout changed while inspecting audio priming");
        for (unsigned i = 0; i < output.value->nb_streams; ++i)
            if (output.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                output.value->streams[i]->codecpar->initial_padding = padding[i];
    }
    detail::writeOutputHeader(output.value, temporaryPath, extension);
    auto freePacket = [](AVPacket* p) { av_packet_free(&p); };
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!packet) throw std::bad_alloc();
    std::size_t written = 0;
    for (;;) {
        detail::checkStopped(stop);
        const int read = av_read_frame(input.context, packet.get());
        if (read == AVERROR_EOF) break;
        detail::check(read, "read packet for stream copy");
        if (packet->stream_index < 0 || static_cast<unsigned>(packet->stream_index) >= output.value->nb_streams)
            throw std::runtime_error("Input stream layout changed during remuxing");
        const auto* incoming = input.context->streams[packet->stream_index];
        const auto* outgoing = output.value->streams[packet->stream_index];
        detail::normalizeCopiedAudioDuration(packet.get(),incoming,detail::declaredEnd(input.context,incoming));
        av_packet_rescale_ts(packet.get(), incoming->time_base, outgoing->time_base);
        packet->pos = -1;
        detail::check(av_interleaved_write_frame(output.value, packet.get()), "write compressed packet");
        ++written;
        av_packet_unref(packet.get());
    }
    detail::check(av_write_trailer(output.value), "finalize output container");
    detail::check(avio_closep(&output.value->pb), "close output file");
    detail::checkStopped(stop);

    const auto verified = probe(temporaryPath, stop);
    std::map<std::pair<std::string, std::string>, unsigned> streamCounts;
    for (const auto& stream : verified.streams) ++streamCounts[{stream.type, stream.codec}];
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        const auto* parameters = input.context->streams[i]->codecpar;
        const auto* kind = av_get_media_type_string(parameters->codec_type);
        auto& count = streamCounts[{kind ? kind : "unknown", avcodec_get_name(parameters->codec_id)}];
        if (count == 0) throw std::runtime_error("output validation found a missing stream");
        --count;
    }
    if (verified.chapters.size() < input.context->nb_chapters) throw std::runtime_error("output container did not preserve all chapters");
    temporary.publish();
    return {input.context->nb_streams, written};
}

} // namespace rifelove::media

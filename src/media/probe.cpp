#include "media/ffmpeg_helpers.hpp"

#include <cmath>

namespace rifelove::media {

std::string MediaEngine::runtimeVersion() { return av_version_info(); }

MediaInfo MediaEngine::probe(const std::string& path, std::stop_token stop) {
    detail::Input input(path, stop);
    auto* context = input.context;
    MediaInfo result{path, context->iformat->name,
        detail::timestamp(context->start_time, AV_TIME_BASE_Q), detail::timestamp(context->duration, AV_TIME_BASE_Q),
        context->bit_rate, detail::metadata(context->metadata), {}, {}};
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        detail::checkStopped(stop);
        const auto* stream = context->streams[i];
        const auto* parameters = stream->codecpar;
        StreamInfo info{};
        info.index = stream->index;
        const auto* type = av_get_media_type_string(parameters->codec_type);
        info.type = type ? type : "unknown";
        info.codec = avcodec_get_name(parameters->codec_id);
        info.timeBase = detail::rational(stream->time_base);
        info.start = detail::timestamp(stream->start_time, stream->time_base);
        info.duration = detail::timestamp(stream->duration, stream->time_base);
        if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) info.averageRate = detail::rational(stream->avg_frame_rate);
        if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) info.nominalRate = detail::rational(stream->r_frame_rate);
        info.declaredFrameCount = stream->nb_frames;
        info.bitRate = parameters->bit_rate;
        info.disposition = stream->disposition;
        info.width = parameters->width;
        info.height = parameters->height;
        info.sampleRate = parameters->sample_rate;
        info.channels = parameters->ch_layout.nb_channels;
        info.metadata = detail::metadata(stream->metadata);
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) info.color = detail::color(parameters);
        for (int j = 0; j < parameters->nb_coded_side_data; ++j) {
            const auto& side = parameters->coded_side_data[j];
            const auto* name = av_packet_side_data_name(side.type);
            info.sideData.push_back({name ? name : "unknown", side.size});
            if (side.type == AV_PKT_DATA_DISPLAYMATRIX && side.size >= 9 * sizeof(std::int32_t)) {
                const double rotation = -av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side.data));
                if (std::isfinite(rotation)) info.rotationDegrees = rotation;
            }
        }
        result.streams.push_back(std::move(info));
    }
    for (unsigned i = 0; i < context->nb_chapters; ++i) {
        const auto* chapter = context->chapters[i];
        result.chapters.push_back({chapter->id, Rational(chapter->start) * detail::rational(chapter->time_base),
            Rational(chapter->end) * detail::rational(chapter->time_base), detail::metadata(chapter->metadata)});
    }
    return result;
}

} // namespace rifelove::media

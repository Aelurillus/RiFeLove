#include "media/ffmpeg_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace rifelove::media {
namespace {
struct Dictionary {
    AVDictionary* value{};
    ~Dictionary() { av_dict_free(&value); }
};
}

std::string MediaEngine::runtimeVersion() { return av_version_info(); }
bool MediaEngine::hasEncoder(const std::string& name) { return avcodec_find_encoder_by_name(name.c_str()) != nullptr; }
EncoderCapability MediaEngine::probeEncoder(const std::string& name, const StreamInfo& source,
    std::optional<Rational> frameRate, std::stop_token stop) {
    EncoderProbeSettings settings;
    settings.frameRate = frameRate;
    return probeEncoderWithSettings(name,source,settings,stop);
}
EncoderCapability MediaEngine::probeEncoderWithSettings(const std::string& name, const StreamInfo& source,
    const EncoderProbeSettings& settings, std::stop_token stop) {
    EncoderCapability result{name,false,false,{}};
    if (name.empty()) throw std::invalid_argument("encoder name must not be empty");
    const auto* encoder = avcodec_find_encoder_by_name(name.c_str());
    if (!encoder || encoder->type != AVMEDIA_TYPE_VIDEO) {
        result.reason = "video encoder is not registered in this FFmpeg build";
        return result;
    }
    result.registered = true;
    try {
        detail::checkStopped(stop);
        if (source.width <= 0 || source.height <= 0) throw std::invalid_argument("source dimensions are unknown");
        if (source.color.pixelFormat.empty()) throw std::invalid_argument("source pixel format is unknown");
        const auto requestedPixelFormat = settings.pixelFormat.empty() ? source.color.pixelFormat : settings.pixelFormat;
        const auto pixelFormat = av_get_pix_fmt(requestedPixelFormat.c_str());
        if (pixelFormat == AV_PIX_FMT_NONE) throw std::invalid_argument("requested pixel format is not recognized by this FFmpeg build");
        const auto rate = settings.frameRate.value_or(source.averageRate.value_or(source.nominalRate.value_or(Rational(30))));
        if (rate <= Rational(0) || rate.numerator() > std::numeric_limits<int>::max() || rate.denominator() > std::numeric_limits<int>::max())
            throw std::invalid_argument("probe frame rate is invalid or out of range");
        if (settings.bitRate < 0 || settings.gopSize < 0 || settings.maxBFrames < 0 || settings.maxBFrames > 16)
            throw std::invalid_argument("probe bitrate, GOP or B-frame setting is invalid");
        const auto timeBase = settings.timeBase.value_or(Rational(rate.denominator(),rate.numerator()));
        if (timeBase <= Rational(0) || timeBase.numerator() > std::numeric_limits<int>::max() ||
            timeBase.denominator() > std::numeric_limits<int>::max())
            throw std::invalid_argument("probe encoder time base is invalid or out of range");
        auto freeCodec = [](AVCodecContext* context) { avcodec_free_context(&context); };
        std::unique_ptr<AVCodecContext,decltype(freeCodec)> context(avcodec_alloc_context3(encoder),freeCodec);
        if (!context) throw std::bad_alloc();
        context->width = source.width;
        context->height = source.height;
        context->pix_fmt = pixelFormat;
        context->framerate = {static_cast<int>(rate.numerator()),static_cast<int>(rate.denominator())};
        context->time_base = {static_cast<int>(timeBase.numerator()),static_cast<int>(timeBase.denominator())};
        context->bit_rate = settings.bitRate;
        context->gop_size = settings.gopSize;
        context->max_b_frames = settings.maxBFrames;
        context->thread_count = 1;
        const void* supported = nullptr;
        int supportedCount = 0;
        detail::check(avcodec_get_supported_config(context.get(),encoder,AV_CODEC_CONFIG_PIX_FORMAT,0,&supported,&supportedCount),
            "query encoder pixel formats");
        if (supported && supportedCount > 0) {
            const auto* formats = static_cast<const AVPixelFormat*>(supported);
            if (std::find(formats,formats + supportedCount,pixelFormat) == formats + supportedCount)
                throw std::invalid_argument("encoder does not support requested pixel format " + requestedPixelFormat);
        }
        detail::checkStopped(stop);
        Dictionary options;
        for (const auto& [key,value] : settings.encoderOptions)
            detail::check(av_dict_set(&options.value,key.c_str(),value.c_str(),0),"configure encoder probe option");
        const auto openStatus = avcodec_open2(context.get(),encoder,&options.value);
        const auto* ignored = av_dict_get(options.value,"",nullptr,AV_DICT_IGNORE_SUFFIX);
        const std::string ignoredOption = ignored ? ignored->key : std::string{};
        detail::check(openStatus,"open video encoder " + name);
        if (!ignoredOption.empty()) throw std::invalid_argument("unsupported encoder option: " + ignoredOption);
        result.contextOpened = true;
    } catch (const std::exception& error) {
        result.reason = error.what();
    }
    return result;
}

MediaInfo MediaEngine::probe(const std::string& path, std::stop_token stop) {
    detail::Input input(path, stop);
    auto* context = input.context;
    MediaInfo result{path, context->iformat->name,
        detail::timestamp(context->start_time, AV_TIME_BASE_Q), detail::timestamp(context->duration, AV_TIME_BASE_Q),
        context->bit_rate, detail::metadata(context->metadata), {}, {}};
    result.preferredVideoStream = av_find_best_stream(context,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
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
        info.declaredEnd = detail::declaredEnd(context,stream);
        info.initialPadding = parameters->initial_padding;
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) info.color = detail::color(parameters);
        for (int j = 0; j < parameters->nb_coded_side_data; ++j) {
            const auto& side = parameters->coded_side_data[j];
            const auto* name = av_packet_side_data_name(side.type);
            info.sideData.push_back({name ? name : "unknown", side.size});
            if (side.type == AV_PKT_DATA_DISPLAYMATRIX && side.size >= 9 * sizeof(std::int32_t)) {
                std::array<std::int32_t, 9> matrix{};
                std::memcpy(matrix.data(),side.data,sizeof(matrix));
                info.displayMatrix = matrix;
                const double rotation = av_display_rotation_get(matrix.data());
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

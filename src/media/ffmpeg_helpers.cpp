#include "media/ffmpeg_helpers.hpp"
#include <charconv>

#include <cerrno>

namespace rifelove::media {

MediaError::MediaError(const std::string& operation, int code)
    : std::runtime_error([&] { char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code, text, sizeof(text)); return operation + ": " + text; }()), code_(code), operation_(operation) {}
bool MediaError::cancelled() const noexcept { return code_ == AVERROR_EXIT; }

namespace detail {
namespace {
std::optional<Rational> durationTag(const std::string& text) {
    const auto colon0 = text.find(':'), colon1 = text.find(':',colon0 == std::string::npos ? 0 : colon0 + 1);
    if (colon0 == std::string::npos || colon1 == std::string::npos) return {};
    const auto point = text.find('.',colon1 + 1);
    auto integer = [&](std::size_t first, std::size_t last) -> std::optional<std::int64_t> {
        if (first >= last || last > text.size()) return {};
        std::int64_t value = 0;
        const auto [end,status] = std::from_chars(text.data() + first,text.data() + last,value);
        if (status != std::errc{} || end != text.data() + last || value < 0) return {};
        return value;
    };
    const auto hours = integer(0,colon0), minutes = integer(colon0 + 1,colon1), seconds = integer(colon1 + 1,point == std::string::npos ? text.size() : point);
    if (!hours || !minutes || !seconds || *minutes >= 60 || *seconds >= 60) return {};
    Rational value;
    try { value = Rational(*hours) * Rational(3600) + Rational(*minutes * 60 + *seconds); }
    catch (const std::overflow_error&) { return {}; }
    if (point != std::string::npos) {
        const auto digits = text.size() - point - 1;
        if (digits == 0 || digits > 9) return {};
        const auto fraction = integer(point + 1,text.size());
        if (!fraction) return {};
        std::int64_t denominator = 1;
        for (std::size_t i = 0; i < digits; ++i) denominator *= 10;
        value = value + Rational(*fraction,denominator);
    }
    return value;
}
}
std::optional<Rational> declaredEnd(const AVFormatContext* input, const AVStream* stream) {
    if (input->iformat && std::string_view(input->iformat->name).find("matroska") != std::string_view::npos) {
        // Matroska has no per-track AVStream duration. During stream-info
        // probing FFmpeg may fill it from the *container* duration (notably
        // for short H.264 tracks). A longer audio tail/CodecDelay must never
        // become the video boundary. Use the muxer's absolute track-end tag;
        // without one, the caller must verify decoded/packet boundaries.
        const auto* tag = av_dict_get(stream->metadata,"DURATION",nullptr,0);
        if (!tag) return {};
        auto end = durationTag(tag->value);
        if (end && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            stream->codecpar->initial_padding > 0 && stream->codecpar->sample_rate > 0) {
            // The tag uses block timestamps, which include CodecDelay. The
            // demuxer subtracts its tick-rounded value from audio packet PTS.
            const auto delay = av_rescale_q(stream->codecpar->initial_padding,AVRational{1,stream->codecpar->sample_rate},stream->time_base);
            end = *end - Rational(delay) * rational(stream->time_base);
        }
        return end;
    }
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const auto start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        return Rational(start) * rational(stream->time_base) + Rational(stream->duration) * rational(stream->time_base);
    }
    return {};
}
void normalizeCopiedAudioDuration(AVPacket* packet, const AVStream* source, std::optional<Rational> end) {
    if (!end || source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || packet->pts == AV_NOPTS_VALUE || packet->duration <= 0) return;
    const auto base = rational(source->time_base), start = Rational(packet->pts) * base;
    if (start >= *end || start + Rational(packet->duration) * base <= *end) return;
    const auto ticks = (*end - start) / base;
    auto rounded = ticks.numerator() / ticks.denominator();
    const auto remainder = ticks.numerator() % ticks.denominator();
    if (remainder >= ticks.denominator() / 2 + ticks.denominator() % 2) ++rounded;
    if (rounded > 0 && rounded < packet->duration) packet->duration = rounded;
}
std::vector<int> initialAudioPadding(const std::string& path, std::stop_token stop) {
    Input probe(path,stop);
    std::vector<int> padding(probe.context->nb_streams,0);
    std::vector<bool> pending(probe.context->nb_streams,false);
    std::size_t remaining = 0;
    for (unsigned i = 0; i < probe.context->nb_streams; ++i) {
        const auto* parameters = probe.context->streams[i]->codecpar;
        padding[i] = parameters->initial_padding;
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO && parameters->initial_padding == 0) { pending[i] = true; ++remaining; }
    }
    const auto freePacket = [](AVPacket* pointer) { av_packet_free(&pointer); };
    std::unique_ptr<AVPacket,decltype(freePacket)> packet(av_packet_alloc(),freePacket);
    if (!packet) throw std::bad_alloc();
    // Streaming scan retains no packet payload. Indexed probing for unusually
    // late-starting audio is a separate performance improvement.
    while (remaining) {
        checkStopped(stop);
        const int status = av_read_frame(probe.context,packet.get());
        if (status == AVERROR_EOF) break;
        check(status,"inspect initial audio padding");
        const auto index = static_cast<unsigned>(packet->stream_index);
        if (index >= pending.size()) throw std::runtime_error("Input stream layout changed while inspecting audio priming");
        if (pending[index]) {
            std::size_t size = 0;
            const auto* data = av_packet_get_side_data(packet.get(),AV_PKT_DATA_SKIP_SAMPLES,&size);
            if (data && size >= 10) {
                const auto samples = static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
                    (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
                if (samples > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) throw std::runtime_error("Audio priming exceeds the supported sample-count range");
                padding[index] = static_cast<int>(samples);
            }
            pending[index] = false; --remaining;
        }
        av_packet_unref(packet.get());
    }
    return padding;
}
namespace {
std::string name(const char* text) { return text ? text : "unknown"; }

ColorInfo makeColor(int format, AVColorPrimaries primaries, AVColorTransferCharacteristic transfer, AVColorSpace matrix, AVColorRange range) {
    ColorInfo result;
    const auto pixelFormat = static_cast<AVPixelFormat>(format);
    result.pixelFormat = name(av_get_pix_fmt_name(pixelFormat));
    if (const auto* description = av_pix_fmt_desc_get(pixelFormat)) result.bitDepth = description->comp[0].depth;
    result.primaries = name(av_color_primaries_name(primaries));
    result.transfer = name(av_color_transfer_name(transfer));
    result.matrix = name(av_color_space_name(matrix));
    result.range = name(av_color_range_name(range));
    result.hdrTransfer = transfer == AVCOL_TRC_SMPTE2084 || transfer == AVCOL_TRC_ARIB_STD_B67;
    return result;
}

void addMastering(ColorInfo& output, const void* data, std::size_t size) {
    if (!data || size < sizeof(AVMasteringDisplayMetadata)) return;
    const auto& input = *static_cast<const AVMasteringDisplayMetadata*>(data);
    MasteringDisplay value;
    if (input.has_primaries) {
        std::array<std::array<Rational, 2>, 3> points;
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = 0; j < 2; ++j) points[i][j] = rational(input.display_primaries[i][j]);
        value.primaries = points;
        value.whitePoint = std::array{rational(input.white_point[0]), rational(input.white_point[1])};
    }
    if (input.has_luminance) {
        value.minLuminance = rational(input.min_luminance);
        value.maxLuminance = rational(input.max_luminance);
    }
    output.masteringDisplay = value;
}
void addLight(ColorInfo& output, const void* data, std::size_t size) {
    if (!data || size < sizeof(AVContentLightMetadata)) return;
    const auto& input = *static_cast<const AVContentLightMetadata*>(data);
    output.contentLight = ContentLight{input.MaxCLL, input.MaxFALL};
}
}

std::filesystem::path pathFromUtf8(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(), value.end())); }
std::string pathToUtf8(const std::filesystem::path& value) {
    const auto text = value.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void check(int code, const std::string& operation) { if (code < 0) throw MediaError(operation, code); }
void checkStopped(std::stop_token stop) { if (stop.stop_requested()) throw MediaError("operation cancelled", AVERROR_EXIT); }
Rational rational(AVRational value) { return Rational(value.num, value.den); }
std::optional<Rational> timestamp(std::int64_t value, AVRational base) {
    if (value == AV_NOPTS_VALUE || base.den <= 0 || base.num <= 0) return std::nullopt;
    return Rational(value) * rational(base);
}
Metadata metadata(const AVDictionary* dictionary) {
    Metadata result;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dictionary, "", entry, AV_DICT_IGNORE_SUFFIX))) result[entry->key] = entry->value;
    return result;
}
ColorInfo color(const AVCodecParameters* p) {
    auto result = makeColor(p->format, p->color_primaries, p->color_trc, p->color_space, p->color_range);
    for (int i = 0; i < p->nb_coded_side_data; ++i) {
        const auto& item = p->coded_side_data[i];
        if (item.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) addMastering(result, item.data, item.size);
        if (item.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) addLight(result, item.data, item.size);
    }
    return result;
}
ColorInfo color(const AVFrame* frame) {
    auto result = makeColor(frame->format, frame->color_primaries, frame->color_trc, frame->colorspace, frame->color_range);
    if (auto* data = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) addMastering(result, data->data, data->size);
    if (auto* data = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) addLight(result, data->data, data->size);
    return result;
}

Input::Input(const std::string& path, std::stop_token stop) : stop_(stop) {
    checkStopped(stop_);
    if (!std::filesystem::is_regular_file(pathFromUtf8(path))) throw std::invalid_argument("input must be an existing local regular file: " + path);
    if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
        (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
        (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR)
        throw std::runtime_error("FFmpeg runtime/header major versions differ");
    context = avformat_alloc_context();
    if (!context) throw std::bad_alloc();
    context->interrupt_callback = {[](void* data) { return static_cast<std::stop_token*>(data)->stop_requested() ? 1 : 0; }, &stop_};
    AVDictionary* options = nullptr;
    const int optionStatus = av_dict_set(&options, "protocol_whitelist", "file", 0);
    if (optionStatus < 0) {
        av_dict_free(&options);
        avformat_close_input(&context);
        throw MediaError("restrict input to local file protocol", optionStatus);
    }
    const int opened = avformat_open_input(&context, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    try {
        check(opened, "open media input");
        check(avformat_find_stream_info(context, nullptr), "find stream information");
        checkStopped(stop_);
    } catch (...) { avformat_close_input(&context); throw; }
}
Input::~Input() { avformat_close_input(&context); }

} // namespace detail
} // namespace rifelove::media

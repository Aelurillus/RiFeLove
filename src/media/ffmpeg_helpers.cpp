#include "media/ffmpeg_helpers.hpp"

#include <cerrno>

namespace rifelove::media {

MediaError::MediaError(const std::string& operation, int code)
    : std::runtime_error([&] { char text[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code, text, sizeof(text)); return operation + ": " + text; }()), code_(code) {}
bool MediaError::cancelled() const noexcept { return code_ == AVERROR_EXIT; }

namespace detail {
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

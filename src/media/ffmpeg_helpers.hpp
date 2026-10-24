#pragma once

#include "media/media_engine.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#include <filesystem>

namespace rifelove::media::detail {

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value);
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& value);
void check(int code, const std::string& operation);
void checkStopped(std::stop_token stop);
[[nodiscard]] Rational rational(AVRational value);
[[nodiscard]] std::optional<Rational> timestamp(std::int64_t value, AVRational base);
[[nodiscard]] Metadata metadata(const AVDictionary* dictionary);
[[nodiscard]] ColorInfo color(const AVCodecParameters* parameters);
[[nodiscard]] ColorInfo color(const AVFrame* frame);
[[nodiscard]] std::optional<Rational> declaredEnd(const AVFormatContext* input, const AVStream* stream);
// Restore a declared final audio boundary lost by demuxer duration rounding.
// Compressed payload, PTS/DTS and skip/discard side data remain untouched.
void normalizeCopiedAudioDuration(AVPacket* packet, const AVStream* source, std::optional<Rational> end);
// Some demuxers expose leading trim only on their first packet. Recover it
// before a Matroska header, which represents it as track-level CodecDelay.
[[nodiscard]] std::vector<int> initialAudioPadding(const std::string& path, std::stop_token stop);

class Input final {
public:
    explicit Input(const std::string& path, std::stop_token stop);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    AVFormatContext* context{};
private:
    std::stop_token stop_;
};

} // namespace rifelove::media::detail

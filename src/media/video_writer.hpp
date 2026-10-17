#pragma once
#include "media/media_engine.hpp"

namespace rifelove::media {
struct VideoEncodingSettings {
    std::string encoder = "h264_nvenc";
    std::string pixelFormat;
    int videoStreamIndex = -1;
    std::optional<Rational> timeBase;
    std::optional<Rational> frameRate;
    std::int64_t bitRate = 8'000'000;
    int gopSize = 120;
    int maxBFrames = 0;
    bool allowPrecisionLoss = false;
    Metadata encoderOptions;
};
struct VideoWriteResult {
    std::string encoder;
    std::size_t framesSubmitted;
    std::size_t videoPackets;
    std::size_t copiedPackets;
    Rational firstTimestamp;
    Rational endExclusive;
    double elapsedSeconds;
    double processingFps;
};

// Model-independent writer for generated CPU frames at explicit presentation times.
// Other reference-input streams are copied. Without finish, the partial output is removed.
class VideoMuxWriter final {
public:
    VideoMuxWriter(const std::string& referenceInput, const std::string& output,
        const VideoEncodingSettings& settings, std::stop_token stop = {});
    ~VideoMuxWriter();
    VideoMuxWriter(const VideoMuxWriter&) = delete;
    VideoMuxWriter& operator=(const VideoMuxWriter&) = delete;
    void writeFrame(const DecodedFrame& frame, Rational timestamp, Rational duration);
    [[nodiscard]] VideoWriteResult finish();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct EncodeProgress { std::size_t framesSubmitted; Rational timestamp; double processingFps; };
[[nodiscard]] VideoWriteResult transcodeVideo(const std::string& input, const std::string& output,
    const VideoEncodingSettings& settings,
    const std::function<void(const EncodeProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove::media

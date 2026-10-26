#pragma once
#include "media/media_engine.hpp"

namespace rifelove::media {
struct VideoEncodingSettings {
    std::string encoder = "h264_nvenc";
    std::string pixelFormat;
    int videoStreamIndex = -1;
    // Zero preserves the reference stream dimensions. Spatial restoration
    // selects both dimensions explicitly before opening the encoder.
    int outputWidth = 0;
    int outputHeight = 0;
    std::optional<Rational> timeBase;
    std::optional<Rational> frameRate;
    std::int64_t bitRate = 8'000'000;
    int gopSize = 120;
    int maxBFrames = 0;
    bool allowPrecisionLoss = false;
    // Internal segment files omit source audio/subtitles and are muxed with
    // those original streams only once during final assembly.
    bool copyAuxiliaryStreams = true;
    bool requireFirstPacketKeyframe = false;
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
// Reference-input streams are copied by default; internal segment outputs may
// opt out. Without finish, the partial output is removed.
class VideoMuxWriter final {
public:
    VideoMuxWriter(const std::string& referenceInput, const std::string& output,
        const VideoEncodingSettings& settings, std::stop_token stop = {});
    ~VideoMuxWriter();
    VideoMuxWriter(const VideoMuxWriter&) = delete;
    VideoMuxWriter& operator=(const VideoMuxWriter&) = delete;
    [[nodiscard]] Rational outputTimeBase() const;
    void writeFrame(const DecodedFrame& frame, Rational timestamp, Rational duration);
    // Optional stronger validation sees the finalized temporary file before
    // no-replace publication. Throwing/cancellation removes the temporary file.
    [[nodiscard]] VideoWriteResult finish(const std::function<void(const std::string&)>& validate = {});
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct EncodeProgress { std::size_t framesSubmitted; Rational timestamp; double processingFps; };
[[nodiscard]] VideoWriteResult transcodeVideo(const std::string& input, const std::string& output,
    const VideoEncodingSettings& settings,
    const std::function<void(const EncodeProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove::media

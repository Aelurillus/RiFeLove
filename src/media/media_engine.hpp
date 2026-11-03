#pragma once

#include "common/rational.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace rifelove::media {

using Metadata = std::map<std::string, std::string>;

class MediaError : public std::runtime_error {
public:
    MediaError(const std::string& operation, int code);
    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] const std::string& operation() const noexcept { return operation_; }
private:
    int code_;
    std::string operation_;
};

struct SideDataInfo { std::string name; std::size_t bytes; };
struct MasteringDisplay {
    std::optional<std::array<std::array<Rational, 2>, 3>> primaries;
    std::optional<std::array<Rational, 2>> whitePoint;
    std::optional<Rational> minLuminance;
    std::optional<Rational> maxLuminance;
};
struct ContentLight { unsigned maxContentLight; unsigned maxFrameAverageLight; };
struct ColorInfo {
    std::string pixelFormat;
    int bitDepth = 0;
    std::string primaries;
    std::string transfer;
    std::string matrix;
    std::string range;
    bool hdrTransfer = false;
    std::optional<MasteringDisplay> masteringDisplay;
    std::optional<ContentLight> contentLight;
};

struct StreamInfo {
    int index;
    std::string type;
    std::string codec;
    Rational timeBase;
    std::optional<Rational> start;
    std::optional<Rational> duration;
    std::optional<Rational> averageRate;
    std::optional<Rational> nominalRate;
    std::int64_t declaredFrameCount;
    std::int64_t bitRate;
    int disposition;
    int width;
    int height;
    int sampleRate;
    int channels;
    std::string sampleFormat;
    std::string channelLayout;
    bool defaultTrack = false;
    bool forcedTrack = false;
    std::optional<double> rotationDegrees; // FFmpeg display-matrix counterclockwise angle.
    std::optional<std::array<std::int32_t, 9>> displayMatrix; // Preserves rotation, reflection and display offsets.
    ColorInfo color;
    Metadata metadata;
    std::vector<SideDataInfo> sideData;
    // Presentation end after codec delay, when declared by the stream/container.
    std::optional<Rational> declaredEnd;
    int initialPadding = 0;
};
struct EncoderCapability {
    std::string encoder;
    bool registered = false;
    bool contextOpened = false;
    std::string reason;
};
struct EncoderProbeSettings {
    std::string pixelFormat;
    std::optional<Rational> timeBase;
    std::optional<Rational> frameRate;
    std::int64_t bitRate = 8'000'000;
    int gopSize = 120;
    int maxBFrames = 0;
    Metadata encoderOptions;
};
struct ChapterInfo { std::int64_t id; Rational start; Rational end; Metadata metadata; };
struct MediaInfo {
    std::string path;
    std::string format;
    std::optional<Rational> start;
    std::optional<Rational> duration;
    std::int64_t bitRate;
    Metadata metadata;
    std::vector<StreamInfo> streams;
    std::vector<ChapterInfo> chapters;
    int preferredVideoStream = -1;
};

// A decoder-owned plane may be a slice of an AVBufferRef shared by several
// planes.  The extent records that owning allocation and the offset of the
// first visible row within it; it is populated only when the decoder can
// prove the relationship from FFmpeg's retained buffer references.  A raw
// data pointer and stride alone are deliberately insufficient for consumers
// that need bounded reads.
struct FramePlaneExtent {
    const std::uint8_t* allocationBegin{};
    std::size_t allocationBytes = 0;
    std::size_t dataOffset = 0;
};
struct FramePlane {
    const std::uint8_t* data{};
    int stride{};
    std::optional<FramePlaneExtent> extent;
};
// Returns the bytes proven readable from the first row toward increasing
// addresses.  This is the safe input for positive-stride consumers.  A
// negative-stride caller must use the allocation bounds itself or reject the
// layout; it must never infer a span from width and height alone.
[[nodiscard]] inline std::optional<std::size_t> provenBytesFromData(const FramePlane& plane) noexcept {
    if (!plane.data || plane.stride <= 0 || !plane.extent || !plane.extent->allocationBegin || plane.extent->allocationBytes == 0)
        return std::nullopt;
    const auto begin = reinterpret_cast<std::uintptr_t>(plane.extent->allocationBegin);
    const auto bytes = plane.extent->allocationBytes;
    if (begin > std::numeric_limits<std::uintptr_t>::max() - bytes) return std::nullopt;
    const auto data = reinterpret_cast<std::uintptr_t>(plane.data);
    const auto end = begin + bytes;
    if (data < begin || data >= end || data - begin != plane.extent->dataOffset) return std::nullopt;
    return bytes - plane.extent->dataOffset;
}
[[nodiscard]] inline std::optional<std::span<const std::uint8_t>> provenPlaneSpan(const FramePlane& plane) noexcept {
    const auto bytes = provenBytesFromData(plane);
    if (!bytes) return std::nullopt;
    return std::span<const std::uint8_t>(plane.data,*bytes);
}
struct DecodedFrame {
    std::size_t index;
    int width;
    int height;
    std::optional<Rational> timestamp;
    std::optional<Rational> duration;
    bool timestampRecovered = false;
    ColorInfo color;
    std::optional<Rational> sampleAspectRatio;
    std::array<FramePlane, 4> planes;
    // Owns reference-counted decoder storage, including original pixel precision and side data.
    std::shared_ptr<const void> storage;
};
struct DecodeSummary {
    std::size_t frameCount = 0;
    std::size_t missingTimestamps = 0;
    std::size_t recoveredTimestamps = 0;
    std::size_t nonIncreasingTimestamps = 0;
    std::string cadence = "insufficient_data";
    std::string decoder;
    std::string acceleration = "software";
    std::string accelerationFallbackReason;
    std::optional<Rational> firstTimestamp;
    std::optional<Rational> lastTimestamp;
};
enum class DecodeAcceleration { software, preferCuda };
struct DecodeOptions {
    DecodeAcceleration acceleration = DecodeAcceleration::software;
    int deviceOrdinal = 0;
};
struct DecodedPreviewFrame {
    int width = 0;
    int height = 0;
    Rational timestamp;
    std::optional<Rational> duration;
    bool timestampRecovered = false;
    std::optional<Rational> previousTimestamp;
    std::optional<Rational> nextTimestamp;
    ColorInfo color;
    // Explicitly converted SDR display pixels; never aliases decoder storage.
    std::vector<std::uint8_t> rgba;
    // Pixel width / height for this decoded frame, when declared by the media.
    std::optional<Rational> sampleAspectRatio;
};
struct RemuxResult { std::size_t copiedStreams; std::size_t packetsWritten; };
struct VideoSegmentContract {
    std::size_t frameCount = 0;
    Rational firstTimestamp;
    Rational frameDuration;
    // Most segments end on a full CFR tick. The final segment may end early
    // when the source duration is not aligned to the output CFR clock.
    std::optional<Rational> endExclusive;
    int width = 0;
    int height = 0;
    std::string codec;
    // Encoder identity is recorded separately from the media codec so a
    // resumed job can keep every segment byte-stream compatible.
    std::string encoder;
    std::string pixelFormat;
    int bitDepth = 0;
    std::optional<std::string> primaries;
    std::optional<std::string> transfer;
    std::optional<std::string> matrix;
    std::optional<std::string> range;
};
struct VideoSegmentVerification {
    std::size_t frameCount = 0;
    Rational firstTimestamp;
    Rational endExclusive;
    std::string codec;
    std::string pixelFormat;
};
struct VideoSegmentInput {
    std::string path;
    VideoSegmentContract contract;
};
struct VideoSegmentAssemblyResult {
    std::size_t segments = 0;
    std::size_t videoPackets = 0;
    std::size_t auxiliaryPackets = 0;
    Rational firstTimestamp;
    Rational endExclusive;
};

class MediaEngine final {
public:
    [[nodiscard]] static std::string runtimeVersion();
    [[nodiscard]] static bool hasEncoder(const std::string& name);
    // Opens a matching encoder context without writing frames. This detects
    // missing hardware/driver APIs more accurately than encoder registration.
    [[nodiscard]] static EncoderCapability probeEncoder(const std::string& name, const StreamInfo& source,
        std::optional<Rational> frameRate = {}, std::stop_token stop = {});
    [[nodiscard]] static EncoderCapability probeEncoderWithSettings(const std::string& name, const StreamInfo& source,
        const EncoderProbeSettings& settings, std::stop_token stop = {});
    [[nodiscard]] static MediaInfo probe(const std::string& utf8Path, std::stop_token stop = {});
    [[nodiscard]] static DecodeSummary decodeVideo(const std::string& utf8Path,
        const std::function<void(const DecodedFrame&)>& onFrame = {}, int streamIndex = -1,
        std::stop_token stop = {}, DecodeOptions options = {}, std::optional<Rational> startTimestamp = {});
    // Convert one owned decoded SDR frame for display, optionally scaling it
    // down to fit a bounded preview surface. The decoded storage must remain
    // alive for the duration of this call.
    [[nodiscard]] static DecodedPreviewFrame previewFrame(const DecodedFrame& frame,
        int maximumWidth = 8192, int maximumHeight = 8192, std::stop_token stop = {});
    // Sequentially decode and convert each frame for streaming consumers. An
    // optional absolute timestamp seeks to a preceding keyframe, so callbacks
    // may include pre-roll frames before that timestamp. One-frame lookahead
    // supplies exact adjacent presentation timestamps. The callback may move
    // frames into a bounded queue; cancellation uses the same stop token.
    [[nodiscard]] static DecodeSummary decodePreviewVideo(const std::string& utf8Path,
        const std::function<void(DecodedPreviewFrame&&)>& onFrame, int maximumWidth = 1280,
        int maximumHeight = 720, int streamIndex = -1, std::stop_token stop = {}, DecodeOptions options = {},
        std::optional<Rational> startTimestamp = {});
    // Seeks and decodes the nearest presentation frame to an exact rational
    // timestamp. The preview conversion accepts only the validated tagged SDR path.
    [[nodiscard]] static DecodedPreviewFrame decodeFrameNearTime(const std::string& utf8Path,
        Rational timestamp, int streamIndex = -1, std::stop_token stop = {});
    // Fully decodes an internal video-only segment and verifies its media and
    // global-timeline contract before the caller journals or reuses it.
    [[nodiscard]] static VideoSegmentVerification verifyVideoSegment(const std::string& utf8Path,
        const VideoSegmentContract& contract, std::stop_token stop = {});
    // Stream-copies verified video segments and the original auxiliary streams
    // into a new output without replacing existing files.
    [[nodiscard]] static VideoSegmentAssemblyResult assembleVideoSegments(const std::string& source,
        const std::string& destination, int videoStreamIndex, const std::vector<VideoSegmentInput>& segments,
        std::stop_token stop = {}, const std::function<void(const std::string&)>& validate = {},
        const std::function<void(const std::string&,const VideoSegmentAssemblyResult&)>& beforePublish = {});
    // Copies compressed streams, metadata and chapters; never replaces an existing output.
    [[nodiscard]] static RemuxResult remux(const std::string& input, const std::string& output,
        std::stop_token stop = {});
};

} // namespace rifelove::media

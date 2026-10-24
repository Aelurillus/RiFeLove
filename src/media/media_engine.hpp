#pragma once

#include "common/rational.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
    std::optional<double> rotationDegrees;
    ColorInfo color;
    Metadata metadata;
    std::vector<SideDataInfo> sideData;
    // Presentation end after codec delay, when declared by the stream/container.
    std::optional<Rational> declaredEnd;
    int initialPadding = 0;
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

struct FramePlane { const std::uint8_t* data{}; int stride{}; };
struct DecodedFrame {
    std::size_t index;
    int width;
    int height;
    std::optional<Rational> timestamp;
    std::optional<Rational> duration;
    ColorInfo color;
    std::array<FramePlane, 4> planes;
    // Owns reference-counted decoder storage, including original pixel precision and side data.
    std::shared_ptr<const void> storage;
};
struct DecodeSummary {
    std::size_t frameCount = 0;
    std::size_t missingTimestamps = 0;
    std::size_t nonIncreasingTimestamps = 0;
    std::string cadence = "insufficient_data";
    std::optional<Rational> firstTimestamp;
    std::optional<Rational> lastTimestamp;
};
struct RemuxResult { std::size_t copiedStreams; std::size_t packetsWritten; };

class MediaEngine final {
public:
    [[nodiscard]] static std::string runtimeVersion();
    [[nodiscard]] static bool hasEncoder(const std::string& name);
    [[nodiscard]] static MediaInfo probe(const std::string& utf8Path, std::stop_token stop = {});
    [[nodiscard]] static DecodeSummary decodeVideo(const std::string& utf8Path,
        const std::function<void(const DecodedFrame&)>& onFrame = {}, int streamIndex = -1, std::stop_token stop = {});
    // Copies compressed streams, metadata and chapters; never replaces an existing output.
    [[nodiscard]] static RemuxResult remux(const std::string& input, const std::string& output,
        std::stop_token stop = {});
};

} // namespace rifelove::media

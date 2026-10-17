#include "media/media_engine.hpp"
#include "media/video_writer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

using rifelove::media::DecodedFrame;
using rifelove::media::MediaEngine;
using rifelove::media::MediaError;

namespace {
class ImageFixture {
public:
    ImageFixture() {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() / ("rifelove-media-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("test directory collision");
        path = directory / "image.ppm";
        std::ofstream file(path, std::ios::binary);
        file << "P6\n2 1\n255\n";
        const std::array<unsigned char, 6> pixels{255, 0, 0, 0, 255, 0};
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file) throw std::runtime_error("could not create image fixture");
    }
    ~ImageFixture() {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) std::filesystem::remove(entry.path(), error);
        std::filesystem::remove(directory, error);
    }
    std::filesystem::path directory;
    std::filesystem::path path;
};
}

TEST(MediaEngine, DecodedPixelsRemainOwnedAfterDecoderCloses) {
    ImageFixture fixture;
    const auto info = MediaEngine::probe(fixture.path.string());
    ASSERT_EQ(info.streams.size(), 1U);
    EXPECT_EQ(info.streams[0].codec, "ppm");
    EXPECT_EQ(info.streams[0].width, 2);
    DecodedFrame retained{};
    const auto summary = MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    EXPECT_EQ(summary.frameCount, 1U);
    ASSERT_TRUE(retained.storage);
    ASSERT_NE(retained.planes[0].data, nullptr);
    EXPECT_EQ(retained.color.pixelFormat, "rgb24");
    EXPECT_EQ(retained.color.bitDepth, 8);
    EXPECT_EQ(retained.planes[0].data[0], 255);
    EXPECT_EQ(retained.planes[0].data[4], 255);
}

TEST(MediaEngine, CancellationIsExplicitAndRetainedFramesStayValid) {
    ImageFixture fixture;
    std::stop_source source;
    DecodedFrame retained{};
    try {
        (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) {
            retained = frame;
            source.request_stop();
        }, -1, source.get_token());
        FAIL() << "expected cancellation";
    } catch (const MediaError& error) { EXPECT_TRUE(error.cancelled()); }
    ASSERT_TRUE(retained.storage);
    EXPECT_EQ(retained.planes[0].data[0], 255);
    EXPECT_THROW((void)MediaEngine::probe(fixture.path.string(), source.get_token()), MediaError);
    const auto output = fixture.directory / "cancelled.mp4";
    EXPECT_THROW((void)MediaEngine::remux(fixture.path.string(), output.string(), source.get_token()), MediaError);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(MediaEngine, RejectsNetworkInputsBeforeOpeningDemuxer) {
    EXPECT_THROW((void)MediaEngine::probe("https://example.invalid/video.mp4"), std::invalid_argument);
}

TEST(VideoMuxWriter, WritesGeneratedFramesAtExplicitTimes) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1, 50);
    settings.frameRate = rifelove::Rational(50);
    const auto output = fixture.directory / "generated.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(), output.string(), settings);
    for (int i = 0; i < 3; ++i) writer.writeFrame(retained, rifelove::Rational(i, 50), rifelove::Rational(1, 50));
    const auto result = writer.finish();
    EXPECT_EQ(result.framesSubmitted, 3U);
    EXPECT_EQ(result.endExclusive, rifelove::Rational(3, 50));
    const auto decoded = MediaEngine::decodeVideo(output.string());
    EXPECT_EQ(decoded.frameCount, 3U);
    EXPECT_EQ(decoded.firstTimestamp, rifelove::Rational(0));
    EXPECT_EQ(decoded.lastTimestamp, rifelove::Rational(1, 25));
    EXPECT_THROW((void)writer.finish(), std::logic_error);
    EXPECT_EQ(retained.planes[0].data[0], 255);
}

TEST(VideoMuxWriter, InvalidTimingAndCancellationRemovePartialFiles) {
    ImageFixture fixture;
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(fixture.path.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1, 50);
    settings.frameRate = rifelove::Rational(50);
    const auto output = fixture.directory / "invalid.mkv";
    rifelove::media::VideoMuxWriter invalid(fixture.path.string(), output.string(), settings);
    EXPECT_THROW(invalid.writeFrame(retained, rifelove::Rational(1, 3), rifelove::Rational(1, 50)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::stop_source stop;
    const auto cancelled = fixture.directory / "cancelled.mkv";
    rifelove::media::VideoMuxWriter writer(fixture.path.string(), cancelled.string(), settings, stop.get_token());
    writer.writeFrame(retained, rifelove::Rational(0), rifelove::Rational(1, 50));
    stop.request_stop();
    EXPECT_THROW(writer.writeFrame(retained, rifelove::Rational(1, 50), rifelove::Rational(1, 50)), MediaError);
    EXPECT_FALSE(std::filesystem::exists(cancelled));
    EXPECT_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(fixture.directory), std::filesystem::directory_iterator())), 1U);
}

TEST(VideoMuxWriter, FractionalRateAndLastDurationRemainExactAcrossAThousandFrames) {
    ImageFixture fixture;
    const auto input = fixture.directory / "source.y4m";
    {
        std::ofstream file(input, std::ios::binary);
        file << "YUV4MPEG2 W16 H16 F24000:1001 Ip A1:1 C420jpeg\nFRAME\n";
        std::vector<unsigned char> pixels(384, 128);
        std::fill(pixels.begin(), pixels.begin() + 256, static_cast<unsigned char>(90));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }
    DecodedFrame retained{};
    (void)MediaEngine::decodeVideo(input.string(), [&](const DecodedFrame& frame) { retained = frame; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "libopenh264";
    settings.timeBase = rifelove::Rational(1, 60000);
    settings.frameRate = rifelove::Rational(60000, 1001);
    settings.bitRate = 300000;
    const auto output = fixture.directory / "fractional.mp4";
    rifelove::media::VideoMuxWriter writer(input.string(), output.string(), settings);
    for (int i = 0; i < 1000; ++i) writer.writeFrame(retained, rifelove::Rational(i * 1001, 60000), rifelove::Rational(1001, 60000));
    const auto result = writer.finish();
    EXPECT_EQ(result.endExclusive, rifelove::Rational(1001, 60));
    std::size_t frames = 0;
    const auto summary = MediaEngine::decodeVideo(output.string(), [&](const DecodedFrame& frame) {
        EXPECT_EQ(frame.timestamp, rifelove::Rational(static_cast<std::int64_t>(frames) * 1001, 60000));
        ++frames;
    });
    EXPECT_EQ(summary.frameCount, 1000U);
    const auto info = MediaEngine::probe(output.string());
    ASSERT_FALSE(info.streams.empty());
    EXPECT_EQ(info.streams[0].duration, result.endExclusive);
}

#include "media/media_engine.hpp"

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
        std::filesystem::remove(path, error);
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

#include "media/encoder_policy.hpp"

#include <gtest/gtest.h>

TEST(AutomaticEncoderPolicy, PlatformContainersKeepAutomaticFallbackOnH264) {
    for (const auto* path : {"clip.mp4", "CLIP.MOV", "clip.m4v", "C:/renders/混合路径/CLIP.MP4"}) {
        EXPECT_TRUE(rifelove::media::automaticEncodingRequiresH264(path)) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "h264_nvenc")) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "libopenh264")) << path;
        EXPECT_FALSE(rifelove::media::isAutomaticEncoderCandidate(path, "libkvazaar")) << path;
        EXPECT_FALSE(rifelove::media::isAutomaticEncoderCandidate(path, "libaom-av1")) << path;
        EXPECT_EQ(rifelove::media::automaticSoftwareEncoderFallbacks(path),
            (std::vector<std::string_view>{"libopenh264"})) << path;
        EXPECT_EQ(rifelove::media::automaticOutputPixelFormat(path), "yuv420p") << path;
    }
}

TEST(AutomaticEncoderPolicy, OtherContainersKeepTheGeneralAutomaticFallbackOrder) {
    for (const auto* path : {"clip.mkv", "clip.webm", "C:/folder.with.dot/clip.avi", "clip"}) {
        EXPECT_FALSE(rifelove::media::automaticEncodingRequiresH264(path)) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "h264_nvenc")) << path;
        EXPECT_EQ(rifelove::media::automaticSoftwareEncoderFallbacks(path),
            (std::vector<std::string_view>{"libopenh264", "libkvazaar", "libaom-av1"})) << path;
        EXPECT_TRUE(rifelove::media::automaticOutputPixelFormat(path).empty()) << path;
    }
}

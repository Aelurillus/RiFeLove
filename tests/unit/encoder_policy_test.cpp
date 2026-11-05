#include "media/encoder_policy.hpp"

#include <gtest/gtest.h>

TEST(AutomaticEncoderPolicy, PlatformContainersKeepAutomaticFallbackOnH264) {
    for (const auto* path : {"clip.mp4", "CLIP.MOV", "clip.m4v", "C:/renders/混合路径/CLIP.MP4"}) {
        EXPECT_TRUE(rifelove::media::automaticEncodingRequiresH264(path)) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "h264_nvenc")) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "libopenh264")) << path;
        EXPECT_FALSE(rifelove::media::isAutomaticEncoderCandidate(path, "hevc_nvenc")) << path;
        EXPECT_FALSE(rifelove::media::isAutomaticEncoderCandidate(path, "av1_nvenc")) << path;
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
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "hevc_nvenc")) << path;
        EXPECT_TRUE(rifelove::media::isAutomaticEncoderCandidate(path, "av1_nvenc")) << path;
        EXPECT_EQ(rifelove::media::automaticSoftwareEncoderFallbacks(path),
            (std::vector<std::string_view>{"libopenh264", "libkvazaar", "libaom-av1"})) << path;
        EXPECT_EQ(rifelove::media::automaticEncoderFallbacks(path),
            (std::vector<std::string_view>{"hevc_nvenc", "av1_nvenc", "libopenh264", "libkvazaar", "libaom-av1"})) << path;
        EXPECT_TRUE(rifelove::media::automaticOutputPixelFormat(path).empty()) << path;
    }
}

TEST(AutomaticEncoderPolicy, HighBitDepthNvencUsesSupportedEquivalentLayouts) {
    using rifelove::media::automaticHardwareEncoderPixelFormat;
    using rifelove::media::effectiveOutputPixelFormat;
    using rifelove::media::expectedDecodedOutputPixelFormat;
    EXPECT_EQ(automaticHardwareEncoderPixelFormat("hevc_nvenc", "yuv444p10le"), "yuv444p10msble");
    EXPECT_EQ(automaticHardwareEncoderPixelFormat("av1_nvenc", "yuv420p10le"), "p010le");
    EXPECT_TRUE(automaticHardwareEncoderPixelFormat("hevc_nvenc", "yuv422p10le").empty());
    EXPECT_EQ(effectiveOutputPixelFormat("clip.mkv", "hevc_nvenc", "yuv444p10le"), "yuv444p10msble");
    EXPECT_EQ(effectiveOutputPixelFormat("clip.mkv", "libaom-av1", "yuv444p10le"), "yuv444p10le");
    EXPECT_EQ(effectiveOutputPixelFormat("clip.mp4", "hevc_nvenc", "yuv444p10le"), "yuv420p");
    EXPECT_EQ(expectedDecodedOutputPixelFormat("hevc_nvenc", "yuv444p10le", "yuv444p10msble"), "yuv444p10le");
    EXPECT_EQ(expectedDecodedOutputPixelFormat("hevc_nvenc", "yuv444p10le", "yuv420p"), "yuv420p");
}

#include "encoding_settings.hpp"

#include <gtest/gtest.h>

namespace rifelove::desktop {

TEST(EncodingSettings, NvencConstantQualityUsesVbrAndCqOptions) {
    const auto settings = makeVideoEncodingSettings(QStringLiteral("h264_nvenc"),
        {{QStringLiteral("qualityMode"),QStringLiteral("cq")}, {QStringLiteral("quality"),19},
         {QStringLiteral("gopSize"),60}});

    EXPECT_EQ(settings.bitRate,0);
    EXPECT_EQ(settings.gopSize,60);
    EXPECT_EQ(settings.encoderOptions.at("rc"),"vbr");
    EXPECT_EQ(settings.encoderOptions.at("cq"),"19");
}

TEST(EncodingSettings, NvencConstantQualityRejectsOutOfRangeValues) {
    EXPECT_THROW(makeVideoEncodingSettings(QStringLiteral("h264_nvenc"),
        {{QStringLiteral("qualityMode"),QStringLiteral("cq")}, {QStringLiteral("quality"),52}}),
        std::invalid_argument);
}

TEST(EncodingSettings, SoftwareH264DoesNotAcceptNvencConstantQuality) {
    EXPECT_THROW(makeVideoEncodingSettings(QStringLiteral("libopenh264"),
        {{QStringLiteral("qualityMode"),QStringLiteral("cq")}, {QStringLiteral("quality"),19}}),
        std::invalid_argument);
}

} // namespace rifelove::desktop

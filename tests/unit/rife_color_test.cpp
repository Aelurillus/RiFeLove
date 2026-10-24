#include "media/rife_color.hpp"
#include <gtest/gtest.h>
#include <limits>

using namespace rifelove::media;
namespace {
DecodedFrame rgbFixture() {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255,0,0, 0,255,0, 0,0,255, 255,255,255});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color.pixelFormat = "rgb24"; frame.color.bitDepth = 8; frame.color.primaries = "bt709";
    frame.color.transfer = "iec61966-2-1"; frame.color.matrix = "gbr"; frame.color.range = "pc";
    frame.planes[0] = {bytes->data(),6}; frame.storage = bytes;
    return frame;
}
}
TEST(RifeColor, RgbChannelOrderPaddingCropAndStorageAreCorrect) {
    auto prepared = prepareRifeSdr(rgbFixture());
    ASSERT_EQ(prepared.paddedWidth,64); ASSERT_EQ(prepared.paddedHeight,64);
    ASSERT_EQ(prepared.pixels.size(),64U * 64 * 3);
    EXPECT_GT(prepared.pixels[0],0.99F); EXPECT_LT(prepared.pixels[4096],0.01F); EXPECT_LT(prepared.pixels[8192],0.01F);
    EXPECT_GT(prepared.pixels[4096 + 1],0.99F); EXPECT_LT(prepared.pixels[1],0.01F);
    for (std::size_t channel = 0; channel < 3; ++channel) EXPECT_EQ(prepared.pixels[channel * 4096 + 4095],prepared.pixels[channel * 4096 + 65]);
    auto restored = restoreRifeSdr(prepared);
    ASSERT_TRUE(restored.storage); EXPECT_EQ(restored.width,2); EXPECT_EQ(restored.height,2);
    EXPECT_EQ(restored.color.matrix,"gbr"); EXPECT_EQ(restored.color.transfer,"iec61966-2-1");
    EXPECT_EQ(restored.planes[0].data[0],255); EXPECT_EQ(restored.planes[0].data[1],0);
    EXPECT_EQ(restored.planes[0].data[restored.planes[0].stride + 2],255);
}
TEST(RifeColor, Limited709BlackWhiteLevelsAndColorTagsArePreserved) {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{16,16,235,235,128,128});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color = rgbFixture().color; frame.color.pixelFormat = "yuv420p"; frame.color.transfer = "bt709"; frame.color.matrix = "bt709"; frame.color.range = "tv";
    frame.storage = bytes; frame.planes[0] = {bytes->data(),2}; frame.planes[1] = {bytes->data() + 4,1}; frame.planes[2] = {bytes->data() + 5,1};
    const auto prepared = prepareRifeSdr(frame);
    for (std::size_t channel = 0; channel < 3; ++channel) { EXPECT_LT(prepared.pixels[channel * 4096],0.01F); EXPECT_GT(prepared.pixels[channel * 4096 + 64],0.99F); }
    const auto restored = restoreRifeSdr(prepared);
    EXPECT_NEAR(restored.planes[0].data[0],16,1); EXPECT_NEAR(restored.planes[0].data[restored.planes[0].stride],235,1);
    EXPECT_EQ(restored.color.pixelFormat,"yuv420p"); EXPECT_EQ(restored.color.matrix,"bt709"); EXPECT_EQ(restored.color.range,"tv"); EXPECT_FALSE(restored.color.hdrTransfer);
}
TEST(RifeColor, Explicit709MatrixProducesRedFromItsLimitedRangeCodeValues) {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{63,63,63,63,102,240});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color = rgbFixture().color; frame.color.pixelFormat = "yuv420p"; frame.color.transfer = "bt709"; frame.color.matrix = "bt709"; frame.color.range = "tv";
    frame.storage = bytes; frame.planes[0] = {bytes->data(),2}; frame.planes[1] = {bytes->data() + 4,1}; frame.planes[2] = {bytes->data() + 5,1};
    const auto prepared = prepareRifeSdr(frame);
    EXPECT_GT(prepared.pixels[0],0.99F); EXPECT_LT(prepared.pixels[4096],0.015F); EXPECT_LT(prepared.pixels[8192],0.015F);
}
TEST(RifeColor, Bt470BgSmpte170mUses601CoefficientsAndPreservesSourceColorTags) {
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{81,81,81,81,90,240});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color = rgbFixture().color; frame.color.pixelFormat = "yuv420p"; frame.color.primaries = "bt470bg";
    frame.color.transfer = "bt709"; frame.color.matrix = "smpte170m"; frame.color.range = "tv";
    frame.storage = bytes; frame.planes[0] = {bytes->data(),2}; frame.planes[1] = {bytes->data() + 4,1}; frame.planes[2] = {bytes->data() + 5,1};
    EXPECT_NO_THROW(validateRifeSdrColor(frame.color));
    const auto prepared = prepareRifeSdr(frame);
    EXPECT_GT(prepared.pixels[0],0.97F); EXPECT_LT(prepared.pixels[4096],0.03F); EXPECT_LT(prepared.pixels[8192],0.03F);
    const auto restored = restoreRifeSdr(prepared);
    EXPECT_EQ(restored.color.primaries,"bt470bg"); EXPECT_EQ(restored.color.transfer,"bt709");
    EXPECT_EQ(restored.color.matrix,"smpte170m"); EXPECT_EQ(restored.color.range,"tv");
    EXPECT_NEAR(restored.planes[0].data[0],81,1); EXPECT_NEAR(restored.planes[1].data[0],90,1); EXPECT_NEAR(restored.planes[2].data[0],240,1);
}
TEST(RifeColor, Limited709TenBitPreservesPrecisionRangeAndTags) {
    auto samples = std::make_shared<std::vector<std::uint16_t>>(std::initializer_list<std::uint16_t>{64,64,940,940,512,512});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color = rgbFixture().color; frame.color.pixelFormat = "yuv420p10le"; frame.color.bitDepth = 10;
    frame.color.transfer = "bt709"; frame.color.matrix = "bt709"; frame.color.range = "tv";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples->data());
    frame.storage = samples; frame.planes[0] = {bytes,4}; frame.planes[1] = {bytes + 8,2}; frame.planes[2] = {bytes + 10,2};

    EXPECT_NO_THROW(validateRifeSdrColor(frame.color));
    const auto prepared = prepareRifeSdr(frame);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        EXPECT_LT(prepared.pixels[channel * 4096],0.01F);
        EXPECT_GT(prepared.pixels[channel * 4096 + 64],0.99F);
    }
    const auto restored = restoreRifeSdr(prepared);
    ASSERT_TRUE(restored.storage);
    EXPECT_EQ(restored.color.pixelFormat,"yuv420p10le"); EXPECT_EQ(restored.color.bitDepth,10);
    EXPECT_EQ(restored.color.primaries,"bt709"); EXPECT_EQ(restored.color.transfer,"bt709");
    EXPECT_EQ(restored.color.matrix,"bt709"); EXPECT_EQ(restored.color.range,"tv");
    const auto* output = reinterpret_cast<const std::uint16_t*>(restored.planes[0].data);
    EXPECT_NEAR(output[0],64,2); EXPECT_NEAR(output[restored.planes[0].stride / 2],940,2);
    EXPECT_NEAR(*reinterpret_cast<const std::uint16_t*>(restored.planes[1].data),512,2);
    EXPECT_NEAR(*reinterpret_cast<const std::uint16_t*>(restored.planes[2].data),512,2);
    EXPECT_EQ(rifeSdrInputProfile(frame.color),"bt709-sdr10");
}
TEST(RifeColor, TenBitBt601ProfileUsesTheLegacyInputProfile) {
    auto frame = rgbFixture();
    frame.color.pixelFormat = "yuv420p10le"; frame.color.bitDepth = 10; frame.color.primaries = "smpte170m";
    frame.color.transfer = "smpte170m"; frame.color.matrix = "smpte170m"; frame.color.range = "tv";
    EXPECT_EQ(rifeSdrInputProfile(frame.color),"bt470bg-sdr10");
}
TEST(RifeColor, TenBitLimited601CoefficientsProduceRed) {
    auto samples = std::make_shared<std::vector<std::uint16_t>>(std::initializer_list<std::uint16_t>{324,324,324,324,360,960});
    DecodedFrame frame{}; frame.width = 2; frame.height = 2;
    frame.color = rgbFixture().color; frame.color.pixelFormat = "yuv420p10le"; frame.color.bitDepth = 10;
    frame.color.primaries = "bt470bg"; frame.color.transfer = "bt709"; frame.color.matrix = "smpte170m"; frame.color.range = "tv";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples->data());
    frame.storage = samples; frame.planes[0] = {bytes,4}; frame.planes[1] = {bytes + 8,2}; frame.planes[2] = {bytes + 10,2};
    const auto prepared = prepareRifeSdr(frame);
    EXPECT_GT(prepared.pixels[0],0.98F); EXPECT_LT(prepared.pixels[4096],0.04F); EXPECT_LT(prepared.pixels[8192],0.04F);
}
TEST(RifeColor, NormalizesKnownContainerAndBitstreamBt601SdrTagConflict) {
    auto streamColor = rgbFixture().color;
    streamColor.pixelFormat = "yuv420p"; streamColor.bitDepth = 8; streamColor.primaries = "bt470bg";
    streamColor.transfer = "bt709"; streamColor.matrix = "smpte170m"; streamColor.range = "tv";
    auto decodedColor = streamColor;
    decodedColor.transfer = "smpte170m"; decodedColor.matrix = "bt470bg";

    EXPECT_NO_THROW(validateRifeSdrColor(decodedColor));
    const auto normalized = normalizeDecodedSdrColor(decodedColor, streamColor);
    EXPECT_EQ(normalized.primaries, "bt470bg");
    EXPECT_EQ(normalized.transfer, "bt709");
    EXPECT_EQ(normalized.matrix, "smpte170m");
    EXPECT_EQ(normalized.range, "tv");
    EXPECT_NO_THROW(validateRifeSdrColor(normalized));

    auto mismatchedRange = decodedColor; mismatchedRange.range = "pc";
    EXPECT_EQ(normalizeDecodedSdrColor(mismatchedRange, streamColor).range, "pc");
    auto hdr = decodedColor; hdr.transfer = "smpte2084"; hdr.hdrTransfer = true;
    EXPECT_EQ(normalizeDecodedSdrColor(hdr, streamColor).transfer, "smpte2084");
}
TEST(RifeColor, UnknownHdrPrecisionInvalidPixelsAndCancellationFailExplicitly) {
    auto source = rgbFixture(); source.color.primaries = "unknown";
    EXPECT_THROW((void)prepareRifeSdr(source),std::invalid_argument);
    source = rgbFixture(); source.color.hdrTransfer = true;
    EXPECT_THROW((void)prepareRifeSdr(source),std::invalid_argument);
    source = rgbFixture(); source.color.bitDepth = 12;
    EXPECT_THROW((void)prepareRifeSdr(source),std::invalid_argument);
    EXPECT_THROW((void)prepareRifeSdr(rgbFixture(),1),std::invalid_argument);
    auto prepared = prepareRifeSdr(rgbFixture()); prepared.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW((void)restoreRifeSdr(prepared),std::invalid_argument);
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW((void)prepareRifeSdr(rgbFixture(),512 * 1024 * 1024,stop.get_token()),MediaError);
}

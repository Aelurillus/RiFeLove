#include "restoration/stdf_tile_policy.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace p = rifelove::restoration::stdf_tiles;

TEST(StdfTilePolicy, SourceBoundsDoNotWidenTheGraphInput) {
    EXPECT_TRUE(rifelove::restoration::validStdfTiledDimensions(3840, 2160));
    EXPECT_TRUE(rifelove::restoration::validStdfTiledDimensions(2160, 3840));
    EXPECT_TRUE(p::supportsDimensions(854, 480));
    EXPECT_TRUE(p::supportsDimensions(1918, 1078));
    EXPECT_TRUE(p::supportsDimensions(1, 1));
    for (const auto dimensions : {std::array{0, 8}, std::array{-8, 8}, std::array{8, -8},
                                 std::array{3841, 8}, std::array{8, 3841}, std::array{3840, 2161},
                                 std::array{3840, 3840}, std::array{std::numeric_limits<int>::max(), 8}}) {
        EXPECT_FALSE(p::supportsDimensions(dimensions[0], dimensions[1]));
        EXPECT_EQ(p::tileCount(dimensions[0], dimensions[1]), 0U);
        EXPECT_FALSE(p::tileAt(dimensions[0], dimensions[1], 0));
    }
    EXPECT_EQ(p::maximumInputWidth, 384);
    EXPECT_EQ(p::maximumInputHeight, 256);
    EXPECT_EQ(std::string_view(p::policyId), "stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1");
    EXPECT_EQ(std::string_view(p::fullFramePolicyId), "full-frame-no-tiling-v1");
}

TEST(StdfTilePolicy, CoresCoverEveryVisiblePixelExactlyOnceIncludingPortraitAndOddEdges) {
    for (const auto dimensions : {std::array{8, 8}, std::array{1, 1}, std::array{384, 256},
                                 std::array{520, 272}, std::array{854, 480}, std::array{1918, 1078},
                                 std::array{1920, 1080}, std::array{3840, 2160}, std::array{2160, 3840},
                                 std::array{3839, 2159}, std::array{3840, 8},
                                 std::array{3839, 7}, std::array{8, 3840}}) {
        const int width = dimensions[0], height = dimensions[1];
        SCOPED_TRACE(std::to_string(width) + "x" + std::to_string(height));
        std::vector<unsigned char> writes(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < p::tileCount(width, height); ++i) {
            const auto value = p::tileAt(width, height, i);
            ASSERT_TRUE(value);
            const auto tile = *value;
            EXPECT_EQ(tile.inputX % 8, 0);
            EXPECT_EQ(tile.inputY % 8, 0);
            EXPECT_EQ(tile.inputWidth % 8, 0);
            EXPECT_EQ(tile.inputHeight % 8, 0);
            EXPECT_LE(tile.inputWidth, 384);
            EXPECT_LE(tile.inputHeight, 256);
            EXPECT_GE(tile.inputWidth, 8);
            EXPECT_GE(tile.inputHeight, 8);
            EXPECT_GE(tile.inputX, 0);
            EXPECT_GE(tile.inputY, 0);
            EXPECT_LE(tile.inputX + tile.inputWidth, p::alignedDimension(width));
            EXPECT_LE(tile.inputY + tile.inputHeight, p::alignedDimension(height));
            EXPECT_LE(tile.inputX, tile.coreX);
            EXPECT_LE(tile.inputY, tile.coreY);
            EXPECT_GE(tile.inputX + tile.inputWidth, tile.coreX + tile.coreWidth);
            EXPECT_GE(tile.inputY + tile.inputHeight, tile.coreY + tile.coreHeight);
            EXPECT_EQ(tile.coreX - tile.inputX, std::min(tile.coreX, p::halo));
            EXPECT_EQ(tile.coreY - tile.inputY, std::min(tile.coreY, p::halo));
            for (int row = tile.coreY; row < tile.coreY + tile.coreHeight; ++row)
                for (int column = tile.coreX; column < tile.coreX + tile.coreWidth; ++column)
                    ++writes[static_cast<std::size_t>(row) * width + column];
        }
        EXPECT_TRUE(std::all_of(writes.begin(), writes.end(), [](unsigned char count) { return count == 1; }));
        EXPECT_FALSE(p::tileAt(width, height, p::tileCount(width, height)));
    }
    EXPECT_EQ(p::tileCount(3840, 2160), 255U);
    EXPECT_EQ(p::tileCount(2160, 3840), 270U);
}

TEST(StdfTilePolicy, OnlyTheGlobalRightBottomBoundaryIsPadded) {
    const auto first = *p::tileAt(854, 479, 0);
    EXPECT_EQ(first.inputX, 0);
    EXPECT_EQ(first.inputY, 0);
    EXPECT_EQ(first.inputWidth, 320);
    EXPECT_EQ(first.inputHeight, 192);
    const auto interior = *p::tileAt(854, 479, 5);
    EXPECT_EQ(interior.coreX, 256);
    EXPECT_EQ(interior.coreY, 128);
    EXPECT_EQ(interior.inputX, 192);
    EXPECT_EQ(interior.inputY, 64);
    EXPECT_EQ(interior.inputWidth, 384);
    EXPECT_EQ(interior.inputHeight, 256);
    const auto last = *p::tileAt(854, 479, p::tileCount(854, 479) - 1);
    EXPECT_EQ(last.coreX + last.coreWidth, 854);
    EXPECT_EQ(last.coreY + last.coreHeight, 479);
    EXPECT_EQ(last.inputX + last.inputWidth, 856);
    EXPECT_EQ(last.inputY + last.inputHeight, 480);
}

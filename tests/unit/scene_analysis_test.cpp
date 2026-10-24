#include "interpolation/scene_analysis.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <limits>
using namespace rifelove::quality;
namespace { RgbView view(const std::vector<float>& pixels) { return {pixels,16,16,16,256}; } }
TEST(SceneAnalysis, HardCutsAndDuplicatesHaveDistinctConservativeActions) {
    const std::vector<float> black(768,0), white(768,1);
    const auto cut = analyzePair(view(black),view(white));
    EXPECT_EQ(cut.action,PairAction::sceneCutHold); EXPECT_DOUBLE_EQ(cut.meanDifference,1); EXPECT_DOUBLE_EQ(cut.histogramDistance,1);
    EXPECT_EQ(analyzePair(view(black),view(black)).action,PairAction::duplicateHold);
}
TEST(SceneAnalysis, SmallTranslatedObjectRemainsEligibleForInterpolation) {
    std::vector<float> left(768,0), right(768,0);
    for (std::size_t channel = 0; channel < 3; ++channel) { left[channel * 256 + 17] = 1; right[channel * 256 + 18] = 1; }
    const auto moving = analyzePair(view(left),view(right));
    EXPECT_EQ(moving.action,PairAction::interpolate); EXPECT_DOUBLE_EQ(moving.histogramDistance,0);
}
TEST(SceneAnalysis, LargeUnexplainedChangeIsHeldEvenWithIdenticalHistograms) {
    std::vector<float> left(768), right(768);
    for (std::size_t i = 0; i < left.size(); ++i) { left[i] = i % 2 ? 0.0F : 1.0F; right[i] = 1 - left[i]; }
    EXPECT_EQ(analyzePair(view(left),view(right)).action,PairAction::abnormalMotionHold);
}
TEST(SceneAnalysis, PaddingIsIgnoredAndInvalidPixelsAreRejected) {
    std::vector<float> left(48,0), right(48,1);
    for (std::size_t channel = 0; channel < 3; ++channel) for (std::size_t row = 0; row < 2; ++row) for (std::size_t x = 0; x < 2; ++x) right[channel * 16 + row * 4 + x] = 0;
    EXPECT_EQ(analyzePair({left,2,2,4,16},{right,2,2,4,16}).action,PairAction::duplicateHold);
    left[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW((void)analyzePair({left,2,2,4,16},{right,2,2,4,16}),std::invalid_argument);
    EXPECT_THROW((void)analyzePair({left,2,2,1,16},{right,2,2,4,16}),std::invalid_argument);
}

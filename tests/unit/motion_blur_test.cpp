#include "core/pipeline/motion_blur.hpp"

#include <gtest/gtest.h>

#include <array>

using rifelove::Rational;
using rifelove::quality::PairAction;
using namespace rifelove::pipeline;

TEST(MotionBlurSampling, UsesExactSymmetricExposureOffsets) {
    const auto samples = motionBlurSampleTimes(Rational(1,60),Rational(60),180,4);
    ASSERT_EQ(samples.size(),4U);
    EXPECT_EQ(samples[0],Rational(13,960));
    EXPECT_EQ(samples[1],Rational(1,64));
    EXPECT_EQ(samples[2],Rational(17,960));
    EXPECT_EQ(samples[3],Rational(19,960));
}

TEST(MotionBlurSampling, ZeroAngleCollapsesEverySampleToTheCenter) {
    const auto samples = motionBlurSampleTimes(Rational(17,60),Rational(60000,1001),0,8);
    ASSERT_EQ(samples.size(),8U);
    for (const auto& sample : samples) EXPECT_EQ(sample,Rational(17,60));
}

TEST(MotionBlurSampling, BlendingUsesLinearLightTransferRoundTrip) {
    const auto linearHalf = (sdrToLinear(0.0F,"iec61966-2-1") + sdrToLinear(1.0F,"iec61966-2-1")) / 2.0F;
    const auto encodedHalf = linearToSdr(linearHalf,"iec61966-2-1");
    EXPECT_GT(encodedHalf,0.7F);
    EXPECT_NEAR(sdrToLinear(encodedHalf,"iec61966-2-1"),linearHalf,1e-5F);
    EXPECT_NEAR(linearToSdr(sdrToLinear(0.42F,"bt709"),"bt709"),0.42F,1e-5F);
}

TEST(MotionBlurSampling, ResolvesCoincidentSourceFrameWithoutInterpolation) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,12)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto location = resolveMotionBlurSample(timestamps,actions,Rational(1,24));
    EXPECT_TRUE(location.sourceFrame);
    EXPECT_FALSE(location.blocked);
    EXPECT_EQ(location.sourceIndex,1U);
}

TEST(MotionBlurSampling, ResolvesSamplesAcrossAdjacentIntervals) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,8)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto location = resolveMotionBlurSample(timestamps,actions,Rational(1,12));
    EXPECT_FALSE(location.sourceFrame);
    EXPECT_FALSE(location.blocked);
    EXPECT_EQ(location.leftIndex,1U);
    EXPECT_EQ(location.rightIndex,2U);
    EXPECT_EQ(location.timestep,Rational(1,2));
}

TEST(MotionBlurSampling, UsesEdgeFramesAtFirstAndFinalExposureBoundaries) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,12)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto first = resolveMotionBlurSample(timestamps,actions,Rational(-1,240));
    const auto last = resolveMotionBlurSample(timestamps,actions,Rational(1,8));
    EXPECT_TRUE(first.sourceFrame); EXPECT_EQ(first.sourceIndex,0U);
    EXPECT_TRUE(last.sourceFrame); EXPECT_EQ(last.sourceIndex,2U);
}

TEST(MotionBlurSampling, BlocksSamplesAndWindowsAtSceneCutBarriers) {
    const std::array<Rational,4> timestamps{Rational(0),Rational(1,24),Rational(1,12),Rational(1,8)};
    const std::array<PairAction,3> actions{PairAction::interpolate,PairAction::sceneCutHold,PairAction::interpolate};
    const auto inside = resolveMotionBlurSample(timestamps,actions,Rational(1,16));
    EXPECT_TRUE(inside.blocked);
    EXPECT_TRUE(motionBlurWindowCrossesBlockedPair(timestamps,actions,Rational(1,48),Rational(1,10)));
    EXPECT_FALSE(motionBlurWindowCrossesBlockedPair(timestamps,actions,Rational(1,12),Rational(1,10)));
}

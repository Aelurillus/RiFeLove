#include "timeline/timeline.hpp"

#include <gtest/gtest.h>

using rifelove::Rational;
using rifelove::Timeline;

TEST(Timeline, PlansNonIntegerRateAgainstVfrTimestamps) {
    const Timeline timeline({Rational(0), Rational(1, 24), Rational(1, 12), Rational(1, 8)}, Rational(1, 6), Rational(60));
    EXPECT_EQ(timeline.outputFrameCount(), 10U);
    const auto interpolated = timeline.sample(1);
    EXPECT_EQ(interpolated.timestamp, Rational(1, 60));
    EXPECT_EQ(interpolated.frame0, 0U);
    EXPECT_EQ(interpolated.frame1, 1U);
    EXPECT_EQ(interpolated.timestep, Rational(2, 5));
    EXPECT_FALSE(interpolated.sourceFrame);
}

TEST(Timeline, PreservesSourceFramesAndHoldsLastFrameToExclusiveEnd) {
    const Timeline timeline({Rational(5), Rational(21, 4)}, Rational(11, 2), Rational(2));
    EXPECT_EQ(timeline.outputFrameCount(), 1U);
    EXPECT_TRUE(timeline.sample(0).sourceFrame);
    EXPECT_THROW((void)timeline.sample(1), std::out_of_range);

    const Timeline extended({Rational(0), Rational(1, 4)}, Rational(1), Rational(4));
    EXPECT_TRUE(extended.sample(2).terminalHold);
    EXPECT_EQ(extended.sample(2).frame0, 1U);
}

TEST(Timeline, RejectsAmbiguousTimeAxesAndNonPositiveRates) {
    EXPECT_THROW((Timeline({Rational(0), Rational(0)}, Rational(1), Rational(30))), std::invalid_argument);
    EXPECT_THROW((Timeline({Rational(0)}, Rational(1), Rational(0))), std::invalid_argument);
    EXPECT_THROW((Timeline({Rational(0)}, Rational(0), Rational(30))), std::invalid_argument);
}
TEST(CfrClock, FractionalRateDoesNotDriftOverOneHundredThousandFrames) {
    rifelove::CfrClock clock(Rational(5,2),Rational(60000,1001));
    EXPECT_EQ(clock.timestamp(),Rational(5,2));
    for (int i = 0; i < 100000; ++i) clock.advance();
    EXPECT_EQ(clock.index(),100000U);
    EXPECT_EQ(clock.timestamp(),Rational(5,2) + Rational(5005,3));
    EXPECT_EQ(clock.period(),Rational(1001,60000));
    EXPECT_THROW((rifelove::CfrClock({},Rational(0))),std::invalid_argument);
}

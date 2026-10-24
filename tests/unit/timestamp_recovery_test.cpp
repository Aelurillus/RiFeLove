#include "media/timestamp_recovery.hpp"
#include <gtest/gtest.h>

using rifelove::Rational;
using rifelove::media::TimestampRecovery;

TEST(TimestampRecovery, UsesDeclaredStartAndPreviousPositiveFrameDuration) {
    TimestampRecovery recovery(Rational(5,2));
    const auto first = recovery.resolve({},Rational(1,24));
    ASSERT_TRUE(first.timestamp); EXPECT_EQ(*first.timestamp,Rational(5,2)); EXPECT_TRUE(first.recovered);
    const auto second = recovery.resolve({},Rational(1,24));
    ASSERT_TRUE(second.timestamp); EXPECT_EQ(*second.timestamp,Rational(5,2) + Rational(1,24)); EXPECT_TRUE(second.recovered);
    const auto third = recovery.resolve(Rational(8,3),Rational(1,30));
    ASSERT_TRUE(third.timestamp); EXPECT_EQ(*third.timestamp,Rational(8,3)); EXPECT_FALSE(third.recovered);
}

TEST(TimestampRecovery, PreservesVfrByUsingFrameDurationInsteadOfAverageRate) {
    TimestampRecovery recovery;
    const auto first = recovery.resolve(Rational(10),Rational(1,25));
    ASSERT_TRUE(first.timestamp); EXPECT_FALSE(first.recovered);
    const auto second = recovery.resolve({},Rational(1,25));
    ASSERT_TRUE(second.timestamp); EXPECT_EQ(*second.timestamp,Rational(10) + Rational(1,25)); EXPECT_TRUE(second.recovered);
    const auto third = recovery.resolve(Rational(51,5),Rational(1,5));
    ASSERT_TRUE(third.timestamp); EXPECT_EQ(*third.timestamp,Rational(51,5)); EXPECT_FALSE(third.recovered);
    const auto fourth = recovery.resolve({},{});
    ASSERT_TRUE(fourth.timestamp); EXPECT_EQ(*fourth.timestamp,Rational(52,5)); EXPECT_TRUE(fourth.recovered);
}

TEST(TimestampRecovery, LeavesUnprovenTimestampsMissingAndRequiresAReanchoringPts) {
    TimestampRecovery recovery;
    EXPECT_FALSE(recovery.resolve({},Rational(1,30)).timestamp);
    EXPECT_FALSE(recovery.resolve({},Rational(1,30)).timestamp);
    const auto anchored = recovery.resolve(Rational(7,3),{});
    ASSERT_TRUE(anchored.timestamp); EXPECT_EQ(*anchored.timestamp,Rational(7,3)); EXPECT_FALSE(anchored.recovered);
    EXPECT_FALSE(recovery.resolve({},{}).timestamp);
}

TEST(TimestampRecovery, IgnoresNonPositiveDurationsAndUsesObservedPtsDirectly) {
    TimestampRecovery recovery(Rational(0));
    const auto first = recovery.resolve(Rational(1),Rational(0));
    ASSERT_TRUE(first.timestamp); EXPECT_FALSE(first.recovered);
    EXPECT_FALSE(recovery.resolve({},Rational(-1)).timestamp);
    const auto observed = recovery.resolve(Rational(3),Rational(1,20));
    ASSERT_TRUE(observed.timestamp); EXPECT_EQ(*observed.timestamp,Rational(3)); EXPECT_FALSE(observed.recovered);
    const auto recovered = recovery.resolve({},{});
    ASSERT_TRUE(recovered.timestamp); EXPECT_EQ(*recovered.timestamp,Rational(61,20)); EXPECT_TRUE(recovered.recovered);
}

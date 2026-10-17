#include "common/rational.hpp"

#include <gtest/gtest.h>

#include <limits>

using rifelove::Rational;

TEST(Rational, NormalizesAndParsesExactFractions) {
    EXPECT_EQ(Rational::parse("-24/36"), Rational(-2, 3));
    EXPECT_EQ(Rational::parse("60"), Rational(60));
    EXPECT_EQ((Rational(1, 24) + Rational(1, 60)).toString(), "7/120");
}

TEST(Rational, OrdersWithoutCrossProductOverflow) {
    const auto nearOne = std::numeric_limits<std::int64_t>::max();
    EXPECT_LT(Rational(nearOne - 1, nearOne), Rational(nearOne, nearOne - 1));
    EXPECT_LT(Rational(-2, 3), Rational(-1, 2));
}

TEST(Rational, RejectsInvalidAndUnrepresentableValues) {
    EXPECT_THROW(Rational(1, 0), std::invalid_argument);
    EXPECT_THROW((void)Rational::parse("1/2/3"), std::invalid_argument);
    EXPECT_THROW(Rational(std::numeric_limits<std::int64_t>::min(), -1), std::overflow_error);
    EXPECT_THROW(Rational(std::numeric_limits<std::int64_t>::max()) + Rational(1), std::overflow_error);
    EXPECT_THROW(Rational(1) / Rational(std::numeric_limits<std::int64_t>::min()), std::overflow_error);
}

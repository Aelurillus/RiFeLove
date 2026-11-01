#pragma once

#include "common/rational.hpp"

#include <limits>
#include <numeric>
#include <stdexcept>

namespace rifelove {

// A common integer clock represents source timestamps and output frame periods
// exactly. Both encoder preflight and the writer must use the same value.
[[nodiscard]] inline Rational encodingTimeBase(Rational sourceBase, Rational targetRate) {
    if (sourceBase <= Rational(0) || targetRate <= Rational(0))
        throw std::invalid_argument("Source time base and target rate must be positive");
    const auto a = sourceBase.denominator(), b = targetRate.numerator();
    const auto factor = a / std::gcd(a,b);
    if (b > std::numeric_limits<int>::max() || factor > std::numeric_limits<int>::max() / b)
        throw std::invalid_argument("Source and target times need an encoder time base beyond 32-bit range");
    return Rational(1,factor * b);
}

} // namespace rifelove

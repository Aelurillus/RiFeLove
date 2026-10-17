#include "common/rational.hpp"

#include <charconv>
#include <limits>
#include <numeric>

namespace rifelove {
namespace {

std::int64_t checkedMultiply(std::int64_t lhs, std::int64_t rhs) {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (lhs == 0 || rhs == 0) return 0;
    if ((lhs == -1 && rhs == min) || (rhs == -1 && lhs == min)) throw std::overflow_error("rational overflow");
    if (lhs > 0 ? (rhs > 0 ? lhs > max / rhs : rhs < min / lhs)
                : (rhs > 0 ? lhs < min / rhs : lhs < max / rhs)) throw std::overflow_error("rational overflow");
    return lhs * rhs;
}

std::int64_t checkedAdd(std::int64_t lhs, std::int64_t rhs) {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs > max - rhs) || (rhs < 0 && lhs < min - rhs)) throw std::overflow_error("rational overflow");
    return lhs + rhs;
}

std::uint64_t magnitude(std::int64_t value) noexcept {
    return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U : static_cast<std::uint64_t>(value);
}

std::strong_ordering comparePositive(std::uint64_t an, std::uint64_t ad, std::uint64_t bn, std::uint64_t bd) noexcept {
    bool reverse = false;
    for (;;) {
        const auto aq = an / ad;
        const auto bq = bn / bd;
        if (aq != bq) {
            const bool less = aq < bq;
            return (less != reverse) ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        const auto ar = an % ad;
        const auto br = bn % bd;
        if (ar == 0 || br == 0) {
            if (ar == 0 && br == 0) return std::strong_ordering::equal;
            const bool less = ar == 0;
            return (less != reverse) ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        an = ad; ad = ar; bn = bd; bd = br; reverse = !reverse;
    }
}

std::int64_t parseInteger(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) throw std::invalid_argument("invalid rational integer");
    return value;
}

} // namespace

Rational::Rational(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0) throw std::invalid_argument("rational denominator cannot be zero");
    if (denominator == std::numeric_limits<std::int64_t>::min()) throw std::overflow_error("rational denominator is out of range");
    if (denominator < 0) {
        if (numerator == std::numeric_limits<std::int64_t>::min()) throw std::overflow_error("rational numerator is out of range");
        numerator = -numerator;
        denominator = -denominator;
    }
    const auto divisor = std::gcd(magnitude(numerator), static_cast<std::uint64_t>(denominator));
    numerator_ = numerator / static_cast<std::int64_t>(divisor);
    denominator_ = denominator / static_cast<std::int64_t>(divisor);
}

Rational Rational::parse(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return Rational(parseInteger(text));
    if (text.find('/', slash + 1) != std::string_view::npos) throw std::invalid_argument("invalid rational format");
    return Rational(parseInteger(text.substr(0, slash)), parseInteger(text.substr(slash + 1)));
}

std::string Rational::toString() const { return std::to_string(numerator_) + "/" + std::to_string(denominator_); }
long double Rational::toLongDouble() const noexcept { return static_cast<long double>(numerator_) / static_cast<long double>(denominator_); }

Rational operator+(Rational a, Rational b) {
    const auto common = std::gcd(a.denominator_, b.denominator_);
    const auto left = checkedMultiply(a.numerator_, b.denominator_ / common);
    const auto right = checkedMultiply(b.numerator_, a.denominator_ / common);
    return Rational(checkedAdd(left, right), checkedMultiply(a.denominator_ / common, b.denominator_));
}
Rational operator-(Rational a, Rational b) {
    if (b.numerator_ == std::numeric_limits<std::int64_t>::min()) throw std::overflow_error("rational overflow");
    return a + Rational(-b.numerator_, b.denominator_);
}
Rational operator*(Rational a, Rational b) {
    const auto g1 = std::gcd(magnitude(a.numerator_), static_cast<std::uint64_t>(b.denominator_));
    const auto g2 = std::gcd(magnitude(b.numerator_), static_cast<std::uint64_t>(a.denominator_));
    return Rational(checkedMultiply(a.numerator_ / static_cast<std::int64_t>(g1), b.numerator_ / static_cast<std::int64_t>(g2)),
                    checkedMultiply(a.denominator_ / static_cast<std::int64_t>(g2), b.denominator_ / static_cast<std::int64_t>(g1)));
}
Rational operator/(Rational a, Rational b) {
    if (b.numerator_ == 0) throw std::invalid_argument("division by zero");
    if (b.numerator_ == std::numeric_limits<std::int64_t>::min()) throw std::overflow_error("rational overflow");
    return a * Rational(b.denominator_ * (b.numerator_ < 0 ? -1 : 1), static_cast<std::int64_t>(magnitude(b.numerator_)));
}
std::strong_ordering operator<=>(Rational a, Rational b) noexcept {
    if ((a.numerator_ < 0) != (b.numerator_ < 0)) return a.numerator_ < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    const auto result = comparePositive(magnitude(a.numerator_), static_cast<std::uint64_t>(a.denominator_),
                                        magnitude(b.numerator_), static_cast<std::uint64_t>(b.denominator_));
    if (a.numerator_ >= 0) return result;
    if (result == std::strong_ordering::less) return std::strong_ordering::greater;
    if (result == std::strong_ordering::greater) return std::strong_ordering::less;
    return std::strong_ordering::equal;
}

} // namespace rifelove

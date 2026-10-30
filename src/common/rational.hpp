#pragma once

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rifelove {

class Rational final {
public:
    Rational(std::int64_t numerator = 0, std::int64_t denominator = 1);
    [[nodiscard]] static Rational parse(std::string_view text);
    [[nodiscard]] std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] std::int64_t denominator() const noexcept { return denominator_; }
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] long double toLongDouble() const noexcept;

    friend Rational operator+(Rational lhs, Rational rhs);
    friend Rational operator-(Rational lhs, Rational rhs);
    friend Rational operator*(Rational lhs, Rational rhs);
    friend Rational operator/(Rational lhs, Rational rhs);
    friend std::strong_ordering operator<=>(Rational lhs, Rational rhs) noexcept;
    friend bool operator==(Rational lhs, Rational rhs) noexcept = default;

private:
    std::int64_t numerator_;
    std::int64_t denominator_;
};

} // namespace rifelove

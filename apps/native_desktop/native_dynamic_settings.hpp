#pragma once

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rifelove::native_desktop {

// The UI uses a signed percentage for the engine's relative [-1, 1] value.
// This is a bias around the measured value, not a fixed model strength.
[[nodiscard]] inline double nativeRelativeAdjustment(std::wstring_view percentage) {
    std::string text;
    text.reserve(percentage.size());
    for (const wchar_t character : percentage) {
        if (character > 0x7f) throw std::invalid_argument("automatic adjustment must be a number from -100 to 100 percent");
        text.push_back(static_cast<char>(character));
    }
    if (!text.empty() && text.front() == '+') {
        text.erase(text.begin());
        if (!text.empty() && (text.front() == '+' || text.front() == '-'))
            throw std::invalid_argument("automatic adjustment must have only one sign");
    }
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value) || value < -100.0 || value > 100.0)
        throw std::invalid_argument("automatic adjustment must be a number from -100 to 100 percent");
    return value / 100.0;
}

} // namespace rifelove::native_desktop

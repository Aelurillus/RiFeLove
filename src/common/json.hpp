#pragma once

#include <string>
#include <string_view>

namespace rifelove {
inline std::string jsonString(std::string_view input) {
    std::string result = "\"";
    for (const unsigned char value : input) {
        switch (value) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result += digits[value >> 4];
                result += digits[value & 0x0f];
            } else result += static_cast<char>(value);
        }
    }
    result += '"';
    return result;
}
} // namespace rifelove

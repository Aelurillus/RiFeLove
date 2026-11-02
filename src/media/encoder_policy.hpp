#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

namespace rifelove::media {

// MP4/MOV are the default creator-upload containers in the native app. When
// automatic hardware H.264 is unavailable, keep those outputs on software
// H.264 instead of silently changing their codec to HEVC or AV1.
inline bool automaticEncodingRequiresH264(std::string_view destination) noexcept {
    const auto separator = destination.find_last_of("/\\");
    const auto dot = destination.find_last_of('.');
    if (dot == std::string_view::npos || (separator != std::string_view::npos && dot < separator)) return false;
    const auto extension = destination.substr(dot);
    const auto equalsAsciiCaseInsensitive = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto a = static_cast<unsigned char>(left[index]);
            const auto b = static_cast<unsigned char>(right[index]);
            const auto lower = [](unsigned char value) {
                return static_cast<unsigned char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
            };
            if (lower(a) != lower(b)) return false;
        }
        return true;
    };
    return equalsAsciiCaseInsensitive(extension, ".mp4") || equalsAsciiCaseInsensitive(extension, ".mov") ||
        equalsAsciiCaseInsensitive(extension, ".m4v");
}

inline std::vector<std::string_view> automaticSoftwareEncoderFallbacks(std::string_view destination) {
    if (automaticEncodingRequiresH264(destination)) return {"libopenh264"};
    return {"libopenh264", "libkvazaar", "libaom-av1"};
}

inline std::string_view automaticOutputPixelFormat(std::string_view destination) noexcept {
    return automaticEncodingRequiresH264(destination) ? std::string_view("yuv420p") : std::string_view{};
}

inline bool isAutomaticEncoderCandidate(std::string_view destination, std::string_view encoder) {
    if (encoder == "h264_nvenc") return true;
    const auto fallbacks = automaticSoftwareEncoderFallbacks(destination);
    return std::find(fallbacks.begin(), fallbacks.end(), encoder) != fallbacks.end();
}

} // namespace rifelove::media

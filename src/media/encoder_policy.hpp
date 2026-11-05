#pragma once

#include <algorithm>
#include <string>
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

inline std::vector<std::string_view> automaticEncoderFallbacks(std::string_view destination) {
    if (automaticEncodingRequiresH264(destination)) return {"libopenh264"};
    return {"hevc_nvenc", "av1_nvenc", "libopenh264", "libkvazaar", "libaom-av1"};
}

inline std::string_view automaticOutputPixelFormat(std::string_view destination) noexcept {
    return automaticEncodingRequiresH264(destination) ? std::string_view("yuv420p") : std::string_view{};
}

inline std::string automaticHardwareEncoderPixelFormat(std::string_view encoder, std::string_view sourcePixelFormat) {
    if (encoder != "hevc_nvenc" && encoder != "av1_nvenc") return {};
    if (sourcePixelFormat == "yuv420p10le" || sourcePixelFormat == "p010le") return "p010le";
    if (sourcePixelFormat == "yuv444p10le" || sourcePixelFormat == "yuv444p10msble") return "yuv444p10msble";
    return {};
}

inline std::string effectiveOutputPixelFormat(std::string_view destination, std::string_view encoder,
                                               std::string_view sourcePixelFormat,
                                               std::string_view requestedPixelFormat = {}) {
    if (!requestedPixelFormat.empty()) return std::string(requestedPixelFormat);
    const auto containerFormat = automaticOutputPixelFormat(destination);
    if (!containerFormat.empty()) return std::string(containerFormat);
    const auto hardwareFormat = automaticHardwareEncoderPixelFormat(encoder, sourcePixelFormat);
    return hardwareFormat.empty() ? std::string(sourcePixelFormat) : hardwareFormat;
}

inline std::string expectedDecodedOutputPixelFormat(std::string_view encoder, std::string_view sourcePixelFormat,
                                                     std::string_view encoderPixelFormat) {
    if (!encoderPixelFormat.empty() && automaticHardwareEncoderPixelFormat(encoder, sourcePixelFormat) == encoderPixelFormat)
        return std::string(sourcePixelFormat);
    return encoderPixelFormat.empty() ? std::string(sourcePixelFormat) : std::string(encoderPixelFormat);
}

inline bool isAutomaticEncoderCandidate(std::string_view destination, std::string_view encoder) {
    if (encoder == "h264_nvenc") return true;
    const auto fallbacks = automaticEncoderFallbacks(destination);
    return std::find(fallbacks.begin(), fallbacks.end(), encoder) != fallbacks.end();
}

} // namespace rifelove::media

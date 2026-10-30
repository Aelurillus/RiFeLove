#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rifelove::application {
enum class PlatformBitrate { unchanged, youtube, localQuality };

inline int platformOutputHeight(int sourceHeight, int targetHeight, int scaleNumerator, int scaleDenominator) {
    if (sourceHeight <= 0 || targetHeight < 0 || scaleNumerator <= 0 || scaleDenominator <= 0)
        throw std::invalid_argument("Invalid dimensions for platform bitrate");
    if (targetHeight > 0) return targetHeight;
    const auto height = static_cast<std::int64_t>(sourceHeight) * scaleNumerator / scaleDenominator;
    if (height <= 0 || height > std::numeric_limits<int>::max())
        throw std::invalid_argument("Output height exceeds platform bitrate range");
    return static_cast<int>(height);
}

// Existing local export suggestions; these do not guarantee a platform's
// playback frame rate or prevent its subsequent transcoding.
inline int platformBitrateKbps(PlatformBitrate profile, int height, int rate) {
    if (profile == PlatformBitrate::youtube) {
        if (height >= 4320) return 200000;
        if (height >= 2160) return 68000;
        if (height >= 1440) return 24000;
        if (height >= 1080) return 12000;
        if (height >= 720) return 7500;
        if (height >= 480) return 4000;
        if (height >= 360) return 1500;
        return 1000;
    }
    const int base = height >= 4320 ? 160000 : height >= 2160 ? 70000 : height >= 1440 ? 40000 :
        height >= 1080 ? 20000 : height >= 720 ? 12000 : 8000;
    const double multiplier = std::min(2.0, std::max(1.0, static_cast<double>(rate) / 60.0));
    return std::min(200000, static_cast<int>(std::round(static_cast<double>(base) * multiplier / 1000.0) * 1000.0));
}
} // namespace rifelove::application

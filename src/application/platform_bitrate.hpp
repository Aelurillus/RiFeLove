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
        if (height <= 0 || rate <= 0)
            throw std::invalid_argument("YouTube upload bitrate needs positive height and frame rate");

        // YouTube publishes separate SDR upload guidance for standard-rate
        // (24/25/30) and high-rate (48/50/60) material. For 8K, choose the
        // upper end of the published range; for 4K, choose 45 Mbps at
        // standard rate and 68 Mbps at high rate. Values above 60 FPS use
        // the high-rate recommendation as an encode-quality estimate only;
        // this does not imply a playback-rate guarantee.
        const bool highFrameRate = rate >= 48;
        if (height >= 4320) return highFrameRate ? 240000 : 160000;
        if (height >= 2160) return highFrameRate ? 68000 : 45000;
        if (height >= 1440) return highFrameRate ? 24000 : 16000;
        if (height >= 1080) return highFrameRate ? 12000 : 8000;
        if (height >= 720) return highFrameRate ? 7500 : 5000;
        if (height >= 480) return highFrameRate ? 4000 : 2500;
        if (height >= 360) return highFrameRate ? 1500 : 1000;
        return 1000;
    }
    const int base = height >= 4320 ? 160000 : height >= 2160 ? 70000 : height >= 1440 ? 40000 :
        height >= 1080 ? 20000 : height >= 720 ? 12000 : 8000;
    const double multiplier = std::min(2.0, std::max(1.0, static_cast<double>(rate) / 60.0));
    return std::min(200000, static_cast<int>(std::round(static_cast<double>(base) * multiplier / 1000.0) * 1000.0));
}

// Returns the encoder GOP length in frames. YouTube recommends a closed GOP
// whose length is half the frame rate; the other local compatibility presets
// retain the existing two-second interval because no current official GOP
// requirement was verified for those upload paths.
inline int platformGopSizeFrames(PlatformBitrate profile, int rate) {
    if (profile == PlatformBitrate::unchanged || rate <= 0)
        throw std::invalid_argument("A platform GOP needs a named profile and positive frame rate");
    const std::int64_t frames = profile == PlatformBitrate::youtube
        ? (static_cast<std::int64_t>(rate) + 1) / 2
        : static_cast<std::int64_t>(rate) * 2;
    if (frames <= 0 || frames > std::numeric_limits<int>::max())
        throw std::invalid_argument("Platform GOP exceeds the encoder range");
    return static_cast<int>(frames);
}
} // namespace rifelove::application

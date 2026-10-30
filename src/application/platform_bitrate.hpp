#pragma once
#include "common/rational.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rifelove::application {
enum class PlatformBitrate { unchanged, youtube, localQuality };

// Integer policy buckets round up without adding numerator + denominator,
// which could overflow even for an otherwise representable positive rate.
inline int platformFrameRateCeiling(Rational rate) {
    if (rate <= Rational(0)) throw std::invalid_argument("Platform frame rate must be positive");
    const auto frames = rate.numerator() / rate.denominator() + (rate.numerator() % rate.denominator() != 0);
    if (frames > std::numeric_limits<int>::max())
        throw std::invalid_argument("Platform frame rate exceeds the encoder range");
    return static_cast<int>(frames);
}

inline int platformOutputHeight(int sourceHeight, int targetHeight, int scaleNumerator, int scaleDenominator) {
    if (sourceHeight <= 0 || targetHeight < 0 || scaleNumerator <= 0 || scaleDenominator <= 0)
        throw std::invalid_argument("Invalid dimensions for platform bitrate");
    if (targetHeight > 0) return targetHeight;
    const auto scaled = static_cast<std::int64_t>(sourceHeight) * scaleNumerator;
    if (scaled % scaleDenominator != 0)
        throw std::invalid_argument("Platform output dimensions must be whole pixels");
    const auto height = scaled / scaleDenominator;
    if (height <= 0 || height > std::numeric_limits<int>::max())
        throw std::invalid_argument("Output height exceeds platform bitrate range");
    return static_cast<int>(height);
}

// The resolution bucket is the actual output's SHORT edge, making landscape
// and portrait exports of the same dimensions equivalent. Existing local
// export suggestions do not guarantee a platform's
// playback frame rate or prevent its subsequent transcoding.
inline int platformBitrateKbps(PlatformBitrate profile, int shortEdge, int rate) {
    if ((profile != PlatformBitrate::youtube && profile != PlatformBitrate::localQuality) || shortEdge <= 0 || rate <= 0)
        throw std::invalid_argument("Platform bitrate needs a named profile, positive short edge and frame rate");
    if (profile == PlatformBitrate::youtube) {
        // YouTube publishes separate SDR upload guidance for standard-rate
        // (24/25/30) and high-rate (48/50/60) material. For 8K, choose the
        // upper end of the published range; for 4K, choose 45 Mbps at
        // standard rate and 68 Mbps at high rate. Values above 60 FPS use
        // the high-rate recommendation as an encode-quality estimate only;
        // this does not imply a playback-rate guarantee.
        const bool highFrameRate = rate >= 48;
        if (shortEdge >= 4320) return highFrameRate ? 240000 : 160000;
        if (shortEdge >= 2160) return highFrameRate ? 68000 : 45000;
        if (shortEdge >= 1440) return highFrameRate ? 24000 : 16000;
        if (shortEdge >= 1080) return highFrameRate ? 12000 : 8000;
        if (shortEdge >= 720) return highFrameRate ? 7500 : 5000;
        if (shortEdge >= 480) return highFrameRate ? 4000 : 2500;
        if (shortEdge >= 360) return highFrameRate ? 1500 : 1000;
        return 1000;
    }
    const int base = shortEdge >= 4320 ? 160000 : shortEdge >= 2160 ? 70000 : shortEdge >= 1440 ? 40000 :
        shortEdge >= 1080 ? 20000 : shortEdge >= 720 ? 12000 : 8000;
    const double multiplier = std::min(2.0, std::max(1.0, static_cast<double>(rate) / 60.0));
    return std::min(200000, static_cast<int>(std::round(static_cast<double>(base) * multiplier / 1000.0) * 1000.0));
}

// Returns the encoder GOP length in frames. YouTube recommends a closed GOP
// whose length is half the frame rate; the other local compatibility presets
// retain the existing two-second interval because no current official GOP
// requirement was verified for those upload paths.
inline int platformGopSizeFrames(PlatformBitrate profile, int rate) {
    if ((profile != PlatformBitrate::youtube && profile != PlatformBitrate::localQuality) || rate <= 0)
        throw std::invalid_argument("A platform GOP needs a named profile and positive frame rate");
    const std::int64_t frames = profile == PlatformBitrate::youtube
        ? (static_cast<std::int64_t>(rate) + 1) / 2
        : static_cast<std::int64_t>(rate) * 2;
    if (frames <= 0 || frames > std::numeric_limits<int>::max())
        throw std::invalid_argument("Platform GOP exceeds the encoder range");
    return static_cast<int>(frames);
}

struct PlatformExportSettings {
    int frameRateCeiling;
    int outputWidth;
    int outputHeight;
    int bitRateKbps;
    int gopSizeFrames;
};

// Shared by single-file controls and EACH probed source in a batch. Explicit
// target dimensions override model scale; otherwise use the generated size.
inline PlatformExportSettings platformExportSettings(PlatformBitrate profile, Rational rate,
    int sourceWidth, int sourceHeight, int targetWidth = 0, int targetHeight = 0,
    int scaleNumerator = 1, int scaleDenominator = 1) {
    if ((targetWidth == 0) != (targetHeight == 0))
        throw std::invalid_argument("Platform target dimensions must specify both width and height");
    const int outputWidth = platformOutputHeight(sourceWidth,targetWidth,scaleNumerator,scaleDenominator);
    const int outputHeight = platformOutputHeight(sourceHeight,targetHeight,scaleNumerator,scaleDenominator);
    const int frames = platformFrameRateCeiling(rate);
    return {frames,outputWidth,outputHeight,
        platformBitrateKbps(profile,std::min(outputWidth,outputHeight),frames),platformGopSizeFrames(profile,frames)};
}
} // namespace rifelove::application

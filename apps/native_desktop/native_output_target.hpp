#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>

namespace rifelove::native_desktop {

struct NativeOutputTarget final {
    int width = 0;
    int height = 0;
    bool selected = false;
    bool aspectMatches = false;
    bool generatedEnough = false;
    bool dimensionsEven = false;
    bool dimensionsSupported = false;
    bool nominalSize = false;

    [[nodiscard]] bool usable() const noexcept {
        return selected && aspectMatches && generatedEnough && dimensionsEven && dimensionsSupported;
    }
};

// Choose an even-pixel point on the source's exact aspect-ratio lattice.
// Rounding only the calculated width would silently stretch some sources and
// also be rejected by the pipeline's exact cross-product aspect-ratio check.
// The preset is a nominal short-edge size; show the actual result when the
// nearest exact size differs (e.g. 2560x1080 -> 3456x1458 for nominal 1440p).
[[nodiscard]] inline NativeOutputTarget nativeOutputTarget(int sourceWidth, int sourceHeight, int selection) {
    NativeOutputTarget result;
    if (selection < 1 || selection > 2) return result;
    result.selected = true;
    if (sourceWidth <= 0 || sourceHeight <= 0) return result;

    const std::int64_t divisor = std::gcd(sourceWidth, sourceHeight);
    std::int64_t stepWidth = sourceWidth / divisor;
    std::int64_t stepHeight = sourceHeight / divisor;
    if (stepWidth % 2 || stepHeight % 2) {
        stepWidth *= 2;
        stepHeight *= 2;
    }
    const std::int64_t desired = selection == 1 ? 1440 : 2160;
    const std::int64_t step = std::min(stepWidth, stepHeight);
    const std::int64_t count = std::max<std::int64_t>(1, (desired + step / 2) / step);
    const std::int64_t width = stepWidth * count;
    const std::int64_t height = stepHeight * count;
    result.dimensionsSupported = width <= 16384 && height <= 16384;
    result.width = static_cast<int>(std::min(width, static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    result.height = static_cast<int>(std::min(height, static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    result.dimensionsEven = result.width > 0 && result.height > 0 && result.width % 2 == 0 && result.height % 2 == 0;
    result.aspectMatches = static_cast<std::int64_t>(sourceWidth) * result.height ==
        static_cast<std::int64_t>(sourceHeight) * result.width;
    result.generatedEnough = width <= static_cast<std::int64_t>(sourceWidth) * 2 &&
        height <= static_cast<std::int64_t>(sourceHeight) * 2;
    result.nominalSize = std::min(width, height) == desired;
    return result;
}

} // namespace rifelove::native_desktop

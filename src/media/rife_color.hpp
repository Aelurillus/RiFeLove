#pragma once
#include "media/media_engine.hpp"

namespace rifelove::media {
struct RifeSdrFrame {
    int width;
    int height;
    int paddedWidth;
    int paddedHeight;
    ColorInfo sourceColor;
    // Normalized nonlinear RGB, contiguous NCHW, batch=1. Padding replicates
    // the last row/column; neither transfer function nor primaries are changed.
    std::vector<float> pixels;
};
// Explicit SDR path: tagged 8/10-bit BT.709 or legacy BT.470BG/SMPTE 170M YUV,
// plus tagged full-range RGB. Unknown metadata, HDR and other bit depths fail closed.
void validateRifeSdrColor(const ColorInfo& color);
[[nodiscard]] std::string rifeSdrInputProfile(const ColorInfo& color);
[[nodiscard]] ColorInfo normalizeDecodedSdrColor(const ColorInfo& decodedColor, const ColorInfo& streamColor);
[[nodiscard]] RifeSdrFrame prepareRifeSdr(const DecodedFrame& frame,
    std::size_t floatBufferLimit = 512ULL * 1024 * 1024, std::stop_token stop = {});
[[nodiscard]] DecodedFrame restoreRifeSdr(const RifeSdrFrame& frame, std::stop_token stop = {});
} // namespace rifelove::media

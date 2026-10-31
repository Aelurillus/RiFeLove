#pragma once

#include "media/media_engine.hpp"
#include "restoration/stdf_tile_policy.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace rifelove::native_desktop {

// UI strength is a separate opt-in from the selected local model. Merely
// registering or choosing a default STDF pack must never enable restoration.
inline float nativeCompressionStrength(int selection) {
    constexpr std::array<float, 4> strengths{0.0F, 0.25F, 0.5F, 1.0F};
    if (selection < 0 || selection >= static_cast<int>(strengths.size()))
        throw std::invalid_argument("Invalid native compression restoration strength");
    return strengths[static_cast<std::size_t>(selection)];
}

// Match the strict service/import contract before enqueueing a single input.
// Batch import still checks every file independently through ImportService.
inline std::wstring nativeCompressionSourceIssue(int width, int height, const media::ColorInfo& color, bool tiled = false) {
    if (tiled ? !restoration::validStdfTiledDimensions(width, height) :
        (width <= 0 || height <= 0 || width > 384 || height > 256))
        return tiled ? L"STDF 分块评估需要合格尺寸；最长边 ≤3840、总像素 ≤3840×2160" :
            L"STDF 整帧评估限 384 × 256；高清素材需显式开启分块评估";
    if (color.pixelFormat != "yuv420p" || color.bitDepth != 8)
        return L"STDF 压缩修复需要 8-bit yuv420p 输入";
    if (color.primaries != "bt709" || color.transfer != "bt709" || color.matrix != "bt709" ||
        color.range != "tv" || color.hdrTransfer || color.masteringDisplay || color.contentLight)
        return L"STDF 压缩修复需要明确标记的 BT.709 限幅 SDR8；不支持 HDR/未知色彩";
    return {};
}

} // namespace rifelove::native_desktop

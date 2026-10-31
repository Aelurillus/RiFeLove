#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <optional>

// Pure policy contract: intentionally no Windows, ORT, CUDA or media headers.
// GUI/readiness and persistent job identity can use this in CPU-only builds.
namespace rifelove::restoration::stdf_tiles {

inline constexpr int alignment = 8;
inline constexpr int coreWidth = 256;
inline constexpr int coreHeight = 128;
inline constexpr int halo = 64;
inline constexpr int maximumAxis = 3840;
inline constexpr std::size_t maximumPixels = 3840U * 2160U;
inline constexpr int maximumInputWidth = coreWidth + 2 * halo;
inline constexpr int maximumInputHeight = coreHeight + 2 * halo;
inline constexpr char policyId[] = "stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1";
inline constexpr char fullFramePolicyId[] = "full-frame-no-tiling-v1";

// Includes portrait 4K. Axis and area limits are independent; a 3840-square
// source is not accepted merely because each individual axis fits.
[[nodiscard]] inline constexpr bool supportsDimensions(int width, int height) noexcept {
    return width > 0 && height > 0 && width <= maximumAxis && height <= maximumAxis &&
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= maximumPixels;
}

[[nodiscard]] inline constexpr int alignedDimension(int value) noexcept {
    return value > 0 && value <= maximumAxis ? (value + alignment - 1) / alignment * alignment : 0;
}

struct Tile {
    int coreX = 0, coreY = 0, coreWidth = 0, coreHeight = 0;
    int inputX = 0, inputY = 0, inputWidth = 0, inputHeight = 0;
};

[[nodiscard]] inline constexpr std::size_t tileCount(int width, int height) noexcept {
    if (!supportsDimensions(width, height)) return 0;
    return static_cast<std::size_t>((width + coreWidth - 1) / coreWidth) *
        static_cast<std::size_t>((height + coreHeight - 1) / coreHeight);
}

// Row-major cores form an exact visible-image partition. Non-8-aligned inputs
// use the old whole-frame adapter's single global right/bottom edge replication
// to ceil8. Internal tile edges never add artificial pixels. An independent
// oracle must pad the whole source once by this same policy before inference.
[[nodiscard]] inline constexpr std::optional<Tile> tileAt(int width, int height, std::size_t index) noexcept {
    if (index >= tileCount(width, height)) return {};
    const auto columns = static_cast<std::size_t>((width + coreWidth - 1) / coreWidth);
    Tile tile;
    tile.coreX = static_cast<int>(index % columns) * coreWidth;
    tile.coreY = static_cast<int>(index / columns) * coreHeight;
    tile.coreWidth = std::min(coreWidth, width - tile.coreX);
    tile.coreHeight = std::min(coreHeight, height - tile.coreY);
    tile.inputX = std::max(0, tile.coreX - halo);
    tile.inputY = std::max(0, tile.coreY - halo);
    tile.inputWidth = std::min(alignedDimension(width), tile.coreX + tile.coreWidth + halo) - tile.inputX;
    tile.inputHeight = std::min(alignedDimension(height), tile.coreY + tile.coreHeight + halo) - tile.inputY;
    return tile;
}

} // namespace rifelove::restoration::stdf_tiles

namespace rifelove::restoration {
[[nodiscard]] inline constexpr bool validStdfTiledDimensions(int width, int height) noexcept {
    return stdf_tiles::supportsDimensions(width, height);
}
} // namespace rifelove::restoration

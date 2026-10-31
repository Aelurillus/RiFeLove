#pragma once

#include "restoration/stdf_luma_adapter.hpp"
#include "restoration/stdf_tile_policy.hpp"

// Experimental graph-bound evaluation policy. Learned deformable offsets have
// no theoretical bound; halo64 does not promise arbitrary-content equivalence
// to a hypothetical high-resolution whole-frame inference.
namespace rifelove::validation::stdf::tiled {

namespace policy = restoration::stdf_tiles;

struct Statistics {
    std::size_t tilesPlanned = 0;
    std::size_t tilesCompleted = 0;
    std::size_t peakFloatBufferBytes = 0;
    std::size_t ownedPayloadBytes = 0;
    std::size_t retainedPlaneSpanBytes = 0;
    int maximumInputWidth = 0;
    int maximumInputHeight = 0;
};

class Plan final {
public:
    [[nodiscard]] const media::DecodedFrame& center() const { return context_.center(); }
    [[nodiscard]] std::stop_token stopToken() const { return context_.stop; }
    [[nodiscard]] const ValidatedContext& context() const { return context_; }
    [[nodiscard]] const Statistics& statistics() const { return statistics_; }
    [[nodiscard]] std::size_t maximumTilePixels() const { return maximumTilePixels_; }
    [[nodiscard]] policy::Tile tile(std::size_t index) const {
        const auto value = policy::tileAt(center().width, center().height, index);
        if (!value) throw Error(Failure::invalidRequest, "STDF tile index is outside its immutable plan");
        return *value;
    }
private:
    Plan() = default;
    friend Plan plan(const restoration::Request&, std::span<const BoundedPlanes>, const SceneBounds&,
                     const VerifiedWeights&, Limits);
    ValidatedContext context_;
    Statistics statistics_;
    std::size_t maximumTilePixels_ = 0;
};

inline Plan plan(const restoration::Request& request, std::span<const BoundedPlanes> extents,
                 const SceneBounds& scene, const VerifiedWeights& model, Limits limits = {}) {
    const auto& center = declaredCenter(request, extents, scene);
    if (!policy::supportsDimensions(center.width, center.height))
        throw Error(Failure::memoryBudget, "STDF tiled evaluation needs positive dimensions, each axis <=3840 and visible pixels <=8294400");
    if (limits.maximumWidth <= 0 || limits.maximumHeight <= 0 ||
        limits.maximumWidth > qualifiedWidth || limits.maximumHeight > qualifiedHeight)
        throw Error(Failure::memoryBudget, "STDF tile graph-input limits cannot exceed 384x256");
    Plan result;
    auto& stats = result.statistics_;
    stats.tilesPlanned = policy::tileCount(center.width, center.height);
    for (std::size_t index = 0; index < stats.tilesPlanned; ++index) {
        stopped(request.stop);
        const auto tile = *policy::tileAt(center.width, center.height, index);
        if (tile.inputWidth > limits.maximumWidth || tile.inputHeight > limits.maximumHeight)
            throw Error(Failure::memoryBudget, "STDF fixed tile policy does not fit the configured graph-input limit; halo cannot be reduced");
        stats.maximumInputWidth = std::max(stats.maximumInputWidth, tile.inputWidth);
        stats.maximumInputHeight = std::max(stats.maximumInputHeight, tile.inputHeight);
        result.maximumTilePixels_ = std::max(result.maximumTilePixels_, multiply(tile.inputWidth, tile.inputHeight));
    }
    // Exactly one reusable seven-plane input and one tile prediction, plus
    // visible Y8 output and the already-verified graph. No full-image FP32
    // tensor exists. ORT/device activations, allocator overhead, and decoder
    // storage beyond exposed plane spans remain outside these payload limits.
    stats.peakFloatBufferBytes = multiply(multiply(8, result.maximumTilePixels_), sizeof(float));
    stats.ownedPayloadBytes = add(model.bytes().size(), add(stats.peakFloatBufferBytes, multiply(center.width, center.height)));
    if (stats.ownedPayloadBytes > limits.maximumOwnedPayloadBytes)
        throw Error(Failure::memoryBudget, "STDF tiled graph/input/output payload plan exceeds the host-memory budget");
    result.context_ = validateContext(request, extents, scene, limits.maximumRetainedPlaneSpanBytes);
    stats.retainedPlaneSpanBytes = result.context_.retainedBytes;
    stopped(request.stop);
    return result;
}

[[nodiscard]] inline std::array<std::int64_t, 4> shape(const policy::Tile& tile) {
    return {1, 7, tile.inputHeight, tile.inputWidth};
}

// The index rather than caller-provided geometry selects an immutable tile;
// callers cannot make an unchecked source extent or overlap the copied cores.
inline void packTile(const Plan& layout, std::size_t index, std::span<float> tensor) {
    stopped(layout.stopToken());
    const auto tile = layout.tile(index);
    const auto pixels = multiply(tile.inputWidth, tile.inputHeight);
    if (tensor.size() != multiply(7, pixels))
        throw Error(Failure::invalidRequest, "STDF tile input buffer has the wrong element count");
    const auto& context = layout.context();
    for (std::size_t slot = 0; slot < 7; ++slot) {
        const auto& frame = context.frames.at(context.slots[slot]);
        for (int row = 0; row < tile.inputHeight; ++row) {
            stopped(layout.stopToken());
            const auto* source = frame.planes[0].data +
                static_cast<std::size_t>(std::min(tile.inputY + row, frame.height - 1)) * frame.planes[0].stride;
            auto* destination = tensor.data() + slot * pixels + static_cast<std::size_t>(row) * tile.inputWidth;
            for (int column = 0; column < tile.inputWidth; ++column)
                destination[column] = static_cast<float>(source[std::min(tile.inputX + column, frame.width - 1)]) / 255.0F;
        }
    }
    stopped(layout.stopToken());
}

template<class Inference>
restoration::Result run(const VerifiedWeights& model, const Plan& layout, float strength,
                        Inference&& inference, Statistics* diagnostics = nullptr) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw Error(Failure::invalidRequest, "STDF strength must be finite in [0,1]");
    if (diagnostics) *diagnostics = layout.statistics();
    stopped(layout.stopToken());
    const auto& center = layout.center();
    if (strength == 0) return {center, 0};
    // These are the only image-sized adapter payload allocations. All byte
    // counts and source extents have been checked before reaching this point.
    std::vector<float> input(multiply(7, layout.maximumTilePixels()));
    std::vector<float> prediction(layout.maximumTilePixels());
    auto storage = std::make_shared<RetainedOutput>();
    storage->originalCenter = center.storage;
    storage->luma.resize(multiply(center.width, center.height));
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < layout.statistics().tilesPlanned; ++index) {
        stopped(layout.stopToken());
        const auto tile = layout.tile(index);
        const auto pixels = multiply(tile.inputWidth, tile.inputHeight);
        const auto inputView = std::span<float>(input).first(multiply(7, pixels));
        const auto predictionView = std::span<float>(prediction).first(pixels);
        packTile(layout, index, inputView);
        std::fill(predictionView.begin(), predictionView.end(), std::numeric_limits<float>::quiet_NaN());
        std::invoke(inference, model.bytes(), std::span<const float>(inputView), shape(tile),
                    predictionView, layout.stopToken());
        stopped(layout.stopToken());
        // Validate halo pixels too. A partial native inference may not happen
        // to corrupt the core, but it is still an invalid output contract.
        for (int row = 0; row < tile.inputHeight; ++row) {
            stopped(layout.stopToken());
            const auto rowView = predictionView.subspan(static_cast<std::size_t>(row) * tile.inputWidth, tile.inputWidth);
            if (!std::all_of(rowView.begin(), rowView.end(), [](float value) { return std::isfinite(value); }))
                throw Error(Failure::nonFiniteOutput, "STDF tile returned an incomplete or non-finite luma prediction; full result discarded");
        }
        for (int row = 0; row < tile.coreHeight; ++row) {
            stopped(layout.stopToken());
            const auto destinationOffset = static_cast<std::size_t>(tile.coreY + row) * center.width + tile.coreX;
            const auto* original = center.planes[0].data +
                static_cast<std::size_t>(tile.coreY + row) * center.planes[0].stride + tile.coreX;
            // Convert global coordinates to a tile-local integer offset before
            // pointer arithmetic; an intermediate global-X pointer can escape
            // a short tile even when the final address would be in bounds.
            const auto restoredOffset =
                static_cast<std::size_t>(tile.coreY - tile.inputY + row) * tile.inputWidth +
                static_cast<std::size_t>(tile.coreX - tile.inputX);
            const auto* restored = predictionView.data() + restoredOffset;
            for (int column = 0; column < tile.coreWidth; ++column) {
                const float target = std::clamp(restored[column], 0.0F, 1.0F) * 255.0F;
                storage->luma[destinationOffset + column] = static_cast<std::uint8_t>(std::lround(std::clamp(
                    original[column] + strength * (target - original[column]), 0.0F, 255.0F)));
            }
        }
        if (diagnostics) ++diagnostics->tilesCompleted;
    }
    stopped(layout.stopToken());
    auto output = center; // Exact PTS/index/duration/color/recovery metadata.
    output.storage = storage;
    output.planes[0] = {storage->luma.data(), center.width,
        media::FramePlaneExtent{storage->luma.data(), storage->luma.size(), 0}};
    stopped(layout.stopToken());
    return {std::move(output), std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()};
}

template<class Inference>
restoration::Result run(const VerifiedWeights& model, const restoration::Request& request,
                        std::span<const BoundedPlanes> extents, const SceneBounds& scene,
                        float strength, Inference&& inference, Limits limits = {}, Statistics* diagnostics = nullptr) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw Error(Failure::invalidRequest, "STDF strength must be finite in [0,1]");
    const auto layout = ::rifelove::validation::stdf::tiled::plan(request, extents, scene, model, limits);
    return ::rifelove::validation::stdf::tiled::run(model, layout, strength, std::forward<Inference>(inference), diagnostics);
}

} // namespace rifelove::validation::stdf::tiled

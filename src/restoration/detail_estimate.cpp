#include "restoration/detail_estimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rifelove::restoration {
namespace {
constexpr std::size_t maximumPositions = 65536;

void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Directional-soft-edge analysis cancelled");
}

DynamicSettings dynamicSettings(const AutomaticDetailSettings& settings) {
    if (!std::isfinite(settings.relativeAdjustment) || settings.relativeAdjustment < -1.0 || settings.relativeAdjustment > 1.0 ||
        !std::isfinite(settings.maximumStrengthChangePerSecond) || settings.maximumStrengthChangePerSecond <= 0.0 ||
        !std::isfinite(settings.minimumConfidence) || settings.minimumConfidence < 0.0 || settings.minimumConfidence > 1.0)
        throw std::invalid_argument("Invalid automatic detail-restoration settings");
    DynamicSettings result;
    result.effects[0].enabled = false;
    result.effects[1].relative = settings.relativeAdjustment;
    result.effects[2].enabled = false;
    result.smoothingSeconds = settings.smoothingSeconds;
    result.maximumChangePerSecond = settings.maximumStrengthChangePerSecond;
    result.minimumConfidence = std::max(settings.minimumConfidence,std::numeric_limits<double>::epsilon());
    return result;
}

struct AxisEvidence {
    std::size_t soft = 0;
    std::size_t sharp = 0;
    double softSpread = 0.0;
    std::array<bool,16> regions{};
    std::array<bool,16> sharpRegions{};
};
}

DetailEstimate estimateDirectionalSoftEdges(const media::RifeSdrFrame& frame, std::stop_token stop) {
    cancelled(stop);
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
        frame.paddedWidth < frame.width || frame.paddedHeight < frame.height ||
        frame.paddedWidth > 8192 || frame.paddedHeight > 8192)
        throw std::invalid_argument("Invalid directional-soft-edge analysis dimensions");
    const auto plane = static_cast<std::size_t>(frame.paddedWidth) * static_cast<std::size_t>(frame.paddedHeight);
    if (frame.pixels.size() != plane * 3)
        throw std::invalid_argument("Invalid directional-soft-edge RGB tensor size");
    if (frame.width < 64 || frame.height < 64) return {};

    const auto luma = [&](int x, int y) {
        const auto offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.paddedWidth) + static_cast<std::size_t>(x);
        const auto r = frame.pixels[offset], g = frame.pixels[plane + offset], b = frame.pixels[2 * plane + offset];
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ||
            std::min({r,g,b}) < 0.0F || std::max({r,g,b}) > 1.0F)
            throw std::invalid_argument("Directional-soft-edge analysis requires finite normalized RGB pixels");
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    };
    const auto area = static_cast<std::size_t>(frame.width - 10) * static_cast<std::size_t>(frame.height - 10);
    const auto stride = std::max(2,static_cast<int>(std::ceil(std::sqrt(static_cast<double>(area) / maximumPositions))));
    std::array<AxisEvidence,2> axes;
    std::size_t sampled = 0;
    for (int y = 5; y < frame.height - 5; y += stride) for (int x = 5; x < frame.width - 5; x += stride) {
        if ((sampled++ & 255U) == 0) cancelled(stop);
        for (int axis = 0; axis < 2; ++axis) {
            std::array<double,9> profile{};
            for (int i = -4; i <= 4; ++i)
                profile[static_cast<std::size_t>(i + 4)] = axis == 0 ? luma(x + i,y) : luma(x,y + i);
            const double signedContrast = profile[8] - profile[0];
            const double contrast = std::abs(signedContrast);
            if (contrast < 0.16 || std::min(profile[0],profile[8]) <= 0.02 || std::max(profile[0],profile[8]) >= 0.98)
                continue;
            const double sign = signedContrast < 0.0 ? -1.0 : 1.0;
            std::array<double,8> slope{};
            double maximum = 0.0;
            int distributed = 0;
            bool monotonic = true;
            for (int i = 0; i < 8; ++i) {
                slope[static_cast<std::size_t>(i)] = sign * (profile[static_cast<std::size_t>(i + 1)] - profile[static_cast<std::size_t>(i)]);
                monotonic = monotonic && slope[static_cast<std::size_t>(i)] >= -0.04 * contrast;
                maximum = std::max(maximum,slope[static_cast<std::size_t>(i)]);
                distributed += slope[static_cast<std::size_t>(i)] >= 0.08 * contrast;
            }
            if (!monotonic || slope[0] > 0.14 * contrast || slope[7] > 0.14 * contrast)
                continue;
            const double midpoint = (profile[0] + profile[8]) * 0.5;
            bool centered = false;
            for (int i = 2; i <= 5; ++i) {
                const double first = sign * (profile[static_cast<std::size_t>(i)] - midpoint);
                const double second = sign * (profile[static_cast<std::size_t>(i + 1)] - midpoint);
                centered = centered || (first <= 0.0 && second >= 0.0);
            }
            if (!centered) continue;
            const auto neighboringEdge = [&](int offset) {
                std::array<double,9> neighbor{};
                for (int i = -4; i <= 4; ++i)
                    neighbor[static_cast<std::size_t>(i + 4)] =
                        axis == 0 ? luma(x + i,y + offset) : luma(x + offset,y + i);
                if (sign * (neighbor[8] - neighbor[0]) < 0.5 * contrast) return false;
                double centralRise = 0.0;
                for (int i = 2; i <= 5; ++i)
                    centralRise = std::max(centralRise,sign * (neighbor[static_cast<std::size_t>(i + 1)] -
                        neighbor[static_cast<std::size_t>(i)]));
                return centralRise >= 0.08 * contrast;
            };
            if (!neighboringEdge(-2) || !neighboringEdge(2)) continue;
            auto& evidence = axes[static_cast<std::size_t>(axis)];
            const double concentration = maximum / contrast;
            const auto region = static_cast<std::size_t>(std::min(3,y * 4 / frame.height) * 4 +
                std::min(3,x * 4 / frame.width));
            if (concentration <= 0.30 && distributed >= 3) {
                ++evidence.soft;
                evidence.softSpread += 1.0 - concentration;
                evidence.regions[region] = true;
            } else if (concentration >= 0.65) {
                ++evidence.sharp;
                evidence.sharpRegions[region] = true;
            }
        }
    }
    cancelled(stop);
    const int dominantAxis = axes[0].soft >= axes[1].soft ? 0 : 1;
    const auto& dominant = axes[static_cast<std::size_t>(dominantAxis)];
    const auto& other = axes[static_cast<std::size_t>(1 - dominantAxis)];
    const auto regions = std::count(dominant.regions.begin(),dominant.regions.end(),true);
    int referencedRegions = 0;
    for (std::size_t region = 0; region < dominant.regions.size(); ++region)
        if (dominant.regions[region] && other.sharpRegions[region]) ++referencedRegions;
    if (dominant.soft < 128 || regions < 10 || dominant.soft < 2 * other.soft ||
        dominant.soft < 2 * dominant.sharp || other.sharp < 24 || referencedRegions < 4)
        return {0.0,0.0,sampled};
    const double spread = dominant.softSpread / static_cast<double>(dominant.soft);
    const double strength = std::clamp((spread - 0.45) * 2.0,0.0,1.0);
    const double support = std::min(1.0,static_cast<double>(dominant.soft) / 128.0) *
        std::min(1.0,static_cast<double>(regions) / 10.0);
    const double dominance = static_cast<double>(dominant.soft) /
        static_cast<double>(dominant.soft + other.soft + dominant.sharp);
    return {strength,std::clamp(support * dominance,0.0,1.0),sampled};
}

AutomaticDetailController::AutomaticDetailController(AutomaticDetailSettings settings)
    : settings_(settings),control_(dynamicSettings(settings),automaticDetailAnalyzerId) {}

AutomaticDetailValue AutomaticDetailController::update(Rational timestamp, const DetailEstimate& estimate, bool sceneCut) {
    if (!std::isfinite(estimate.strength) || estimate.strength < 0.0 || estimate.strength > 1.0 ||
        !std::isfinite(estimate.confidence) || estimate.confidence < 0.0 || estimate.confidence > 1.0)
        throw std::invalid_argument("Invalid directional-soft-edge estimate");
    const bool confident = estimate.confidence > 0.0 && estimate.confidence >= settings_.minimumConfidence;
    DynamicObservation observation{timestamp,{},sceneCut};
    observation.effects[1] = {estimate.strength,confident ? estimate.confidence : 0.0};
    const auto value = control_.update(observation);
    return {confident ? static_cast<float>(value.strength[1] * maximumAutomaticDeblurBlend) : 0.0F,confident};
}
} // namespace rifelove::restoration

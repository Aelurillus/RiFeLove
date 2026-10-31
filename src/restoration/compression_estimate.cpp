#include "restoration/compression_estimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rifelove::restoration {
namespace {
constexpr std::size_t maximumSamplesPerAxis = 8192;
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Compression-artifact analysis cancelled");
}
DynamicSettings dynamicSettings(const AutomaticCompressionSettings& settings) {
    if (!std::isfinite(settings.relativeAdjustment) || settings.relativeAdjustment < -1.0 || settings.relativeAdjustment > 1.0 ||
        !std::isfinite(settings.maximumStrengthChangePerSecond) || settings.maximumStrengthChangePerSecond <= 0.0 ||
        !std::isfinite(settings.minimumConfidence) || settings.minimumConfidence < 0.0 || settings.minimumConfidence > 1.0)
        throw std::invalid_argument("Invalid automatic compression-restoration settings");
    DynamicSettings result;
    result.effects[0].relative = settings.relativeAdjustment;
    result.effects[1].enabled = false;
    result.effects[2].enabled = false;
    result.smoothingSeconds = settings.smoothingSeconds;
    result.maximumChangePerSecond = settings.maximumStrengthChangePerSecond;
    result.minimumConfidence = std::max(settings.minimumConfidence,std::numeric_limits<double>::epsilon());
    return result;
}
struct AxisEvidence {
    std::size_t examined = 0, accepted = 0, supported = 0;
    double excess = 0.0;
    std::array<bool,16> regions{};
};
std::size_t stratumOffset(std::size_t seam, std::size_t stratum, std::size_t length) {
    // Unsigned integer mixing is stable across platforms and replay. Varying
    // the position within each cross-axis stratum avoids a shared row lattice.
    auto mixed = static_cast<std::uint32_t>(seam + 1) * 0x9E3779B9U ^
        static_cast<std::uint32_t>(stratum + 1) * 0x85EBCA6BU;
    mixed ^= mixed >> 16;
    mixed *= 0x7FEB352DU;
    mixed ^= mixed >> 15;
    mixed *= 0x846CA68BU;
    mixed ^= mixed >> 16;
    return static_cast<std::size_t>(mixed) % length;
}
}

CompressionEstimate estimateCompressionArtifacts(const media::DecodedFrame& frame, std::stop_token stop) {
    cancelled(stop);
    const auto& color = frame.color;
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
        !frame.storage || color.pixelFormat != "yuv420p" || color.bitDepth != 8 || color.primaries != "bt709" ||
        color.transfer != "bt709" || color.matrix != "bt709" || color.range != "tv" ||
        color.hdrTransfer || color.masteringDisplay || color.contentLight)
        throw std::invalid_argument("Compression analysis requires owned tagged BT.709 TV-range YUV420P SDR8 source pixels");
    const auto span = media::provenPlaneSpan(frame.planes[0]);
    const int stride = frame.planes[0].stride;
    if (!span || stride < frame.width ||
        static_cast<std::size_t>(frame.height - 1) > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(frame.width)) / static_cast<std::size_t>(stride) ||
        span->size() < static_cast<std::size_t>(frame.height - 1) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(frame.width))
        throw std::invalid_argument("Compression analysis luma allocation extent is missing or too small");
    if (frame.width < 32 || frame.height < 32) return {};
    const auto pixel = [&](int x, int y) { return static_cast<int>((*span)[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)]); };
    std::array<AxisEvidence,2> axes;
    for (int axis = 0; axis < 2; ++axis) {
        auto& evidence = axes[static_cast<std::size_t>(axis)];
        const int length = axis == 0 ? frame.width : frame.height;
        const int cross = axis == 0 ? frame.height : frame.width;
        const auto seams = static_cast<std::size_t>((length - 2) / 8);
        const auto crossPositions = static_cast<std::size_t>(cross - 2);
        // Each seam is a separate stratum: never subsample its index with a
        // fixed stride, which can omit all 16/32-pixel boundaries at 1080p.
        // The 8192-pixel dimension cap leaves at most 1023 seams, so every
        // seam fits while cross-axis stratification bounds the total work.
        const auto crossStrata = std::min(crossPositions,maximumSamplesPerAxis / seams);
        for (std::size_t stratum = 0; stratum < crossStrata; ++stratum) {
            const auto begin = stratum * crossPositions / crossStrata;
            const auto end = (stratum + 1) * crossPositions / crossStrata;
            for (std::size_t seam = 0; seam < seams; ++seam) {
                if ((evidence.examined++ & 255U) == 0) cancelled(stop);
                const int along = static_cast<int>(seam + 1) * 8;
                const int across = static_cast<int>(begin + stratumOffset(seam,stratum,end - begin)) + 1;
                const auto read = [&](int a, int b) { return axis == 0 ? pixel(a,b) : pixel(b,a); };
                const int a = read(along - 2,across), b = read(along - 1,across);
                const int c = read(along,across), d = read(along + 1,across);
                if (std::min({a,b,c,d}) <= 17 || std::max({a,b,c,d}) >= 234) continue;
                const double local = (std::abs(b - a) + std::abs(d - c)) * 0.5;
                const double parallel = (std::abs(read(along - 1,across - 1) - read(along - 1,across + 1)) +
                    std::abs(read(along,across - 1) - read(along,across + 1))) * 0.25;
                const int boundary = std::abs(c - b);
                // Strong real edges and busy texture cannot safely condition this
                // model from a spatial block score alone.
                if (local > 12.0 || parallel > 12.0 || boundary > 48) continue;
                ++evidence.accepted;
                const double excess = std::max(0.0,static_cast<double>(boundary) - local - 1.0);
                evidence.excess += std::min(excess,24.0);
                if (excess >= 2.5 && boundary >= local * 1.75 + 2.0) {
                    ++evidence.supported;
                    const int x = axis == 0 ? along : across, y = axis == 0 ? across : along;
                    evidence.regions[static_cast<std::size_t>(std::min(3,y * 4 / frame.height) * 4 + std::min(3,x * 4 / frame.width))] = true;
                }
            }
        }
    }
    cancelled(stop);
    const auto samples = axes[0].accepted + axes[1].accepted;
    if (axes[0].accepted < 64 || axes[1].accepted < 64) return {0.0,0.0,samples};
    const auto mean = [](const AxisEvidence& e) { return e.excess / static_cast<double>(e.accepted); };
    const auto coverage = [](const AxisEvidence& e) { return static_cast<double>(e.accepted) / static_cast<double>(e.examined); };
    const double supportCoverage = std::min(coverage(axes[0]),coverage(axes[1]));
    const double sampleCoverage = std::min(1.0,static_cast<double>(std::min(axes[0].accepted,axes[1].accepted)) / 1024.0);
    if (mean(axes[0]) < 0.5 && mean(axes[1]) < 0.5) {
        // A genuinely clean frame has no accepted block-edge support. A lone
        // strong aligned edge can be diluted below the mean threshold while
        // still being ambiguous, so it must not receive clean-frame confidence.
        const bool sparseEvidence = axes[0].supported > 0 || axes[1].supported > 0;
        return {0.0,sparseEvidence ? 0.0 : supportCoverage * sampleCoverage,samples};
    }
    double confidence = supportCoverage * sampleCoverage;
    for (const auto& evidence : axes) {
        const auto regions = std::count(evidence.regions.begin(),evidence.regions.end(),true);
        confidence *= std::min(1.0,static_cast<double>(evidence.supported) / static_cast<double>(evidence.accepted) / 0.25) *
            std::min(1.0,static_cast<double>(regions) / 6.0);
    }
    const double low = std::min(mean(axes[0]),mean(axes[1])), high = std::max(mean(axes[0]),mean(axes[1]));
    // Requiring both directions and spatially distributed support rejects a
    // lone aligned edge; true grid-like artwork remains inherently ambiguous.
    if (low < 0.5 || low < high * 0.15) confidence = 0.0;
    return {std::clamp(low / 12.0,0.0,1.0),std::clamp(confidence,0.0,1.0),samples};
}

AutomaticCompressionController::AutomaticCompressionController(AutomaticCompressionSettings settings)
    : settings_(settings),control_(dynamicSettings(settings),automaticCompressionAnalyzerId) {}

AutomaticCompressionValue AutomaticCompressionController::update(
    Rational timestamp, const CompressionEstimate& estimate, bool sceneCut) {
    if (!std::isfinite(estimate.strength) || estimate.strength < 0.0 || estimate.strength > 1.0 ||
        !std::isfinite(estimate.confidence) || estimate.confidence < 0.0 || estimate.confidence > 1.0)
        throw std::invalid_argument("Invalid compression-artifact estimate");
    const bool confident = estimate.confidence > 0.0 && estimate.confidence >= settings_.minimumConfidence;
    DynamicObservation observation{timestamp,{},sceneCut};
    observation.effects[0] = {estimate.strength,confident ? estimate.confidence : 0.0};
    const auto value = control_.update(observation);
    return {confident ? static_cast<float>(value.strength[0]) : 0.0F,confident};
}
} // namespace rifelove::restoration

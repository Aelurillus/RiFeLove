#include "interpolation/scene_analysis.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace rifelove::quality {
std::string_view toString(PairAction action) {
    switch (action) {
    case PairAction::interpolate: return "interpolate";
    case PairAction::duplicateHold: return "duplicate_hold";
    case PairAction::sceneCutHold: return "scene_cut_hold";
    case PairAction::abnormalMotionHold: return "abnormal_motion_hold";
    }
    throw std::invalid_argument("Unknown scene action");
}
namespace {
void validate(RgbView view) {
    if (view.width <= 0 || view.height <= 0 || view.width > 8192 || view.height > 8192 || view.rowStride < view.width || view.rowStride > 8192 ||
        view.planeSize < static_cast<std::size_t>(view.rowStride) * static_cast<std::size_t>(view.height) || view.planeSize > view.pixels.size() / 3)
        throw std::invalid_argument("Invalid planar RGB quality view");
}
}
SceneAnalysis analyzePair(RgbView left, RgbView right, const SceneSettings& settings) {
    validate(left); validate(right);
    if (left.width != right.width || left.height != right.height) throw std::invalid_argument("Quality analysis requires matching image dimensions");
    for (const auto value : {settings.cutMeanDifference, settings.cutHistogramDistance, settings.abnormalMeanDifference, settings.duplicateMaximumDifference})
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Quality thresholds must be finite normalized values");
    std::array<std::size_t,48> first{}, second{};
    double total = 0, maximum = 0;
    const auto count = static_cast<std::size_t>(left.width) * static_cast<std::size_t>(left.height) * 3;
    for (std::size_t channel = 0; channel < 3; ++channel) for (int y = 0; y < left.height; ++y) for (int x = 0; x < left.width; ++x) {
        const auto a = left.pixels[channel * left.planeSize + static_cast<std::size_t>(y) * static_cast<std::size_t>(left.rowStride) + static_cast<std::size_t>(x)];
        const auto b = right.pixels[channel * right.planeSize + static_cast<std::size_t>(y) * static_cast<std::size_t>(right.rowStride) + static_cast<std::size_t>(x)];
        if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || a > 1 || b < 0 || b > 1) throw std::invalid_argument("Quality input contains invalid RGB samples");
        const auto difference = std::abs(static_cast<double>(a) - b);
        total += difference; maximum = std::max(maximum,difference);
        ++first[channel * 16 + std::min<std::size_t>(static_cast<std::size_t>(a * 16),15)];
        ++second[channel * 16 + std::min<std::size_t>(static_cast<std::size_t>(b * 16),15)];
    }
    double histogram = 0;
    for (std::size_t i = 0; i < first.size(); ++i) histogram += std::abs(static_cast<double>(first[i]) - static_cast<double>(second[i]));
    const double mean = total / static_cast<double>(count), distance = histogram / (2 * static_cast<double>(count));
    auto action = PairAction::interpolate;
    if (maximum <= settings.duplicateMaximumDifference) action = PairAction::duplicateHold;
    else if (mean >= settings.cutMeanDifference && distance >= settings.cutHistogramDistance) action = PairAction::sceneCutHold;
    else if (mean >= settings.abnormalMeanDifference) action = PairAction::abnormalMotionHold;
    return {action,mean,distance,maximum};
}
} // namespace rifelove::quality

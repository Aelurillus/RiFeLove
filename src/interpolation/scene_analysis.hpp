#pragma once
#include <cstddef>
#include <span>
#include <string_view>

namespace rifelove::quality {
struct RgbView { std::span<const float> pixels; int width; int height; int rowStride; std::size_t planeSize; };
struct SceneSettings {
    double cutMeanDifference = 0.25;
    double cutHistogramDistance = 0.35;
    double abnormalMeanDifference = 0.50;
    double duplicateMaximumDifference = 1e-6;
};
enum class PairAction { interpolate, duplicateHold, sceneCutHold, abnormalMotionHold };
[[nodiscard]] std::string_view toString(PairAction action);
struct SceneAnalysis {
    PairAction action;
    double meanDifference;
    double histogramDistance;
    double maximumDifference;
};
// First conservative CPU analyzer, independent of codecs and inference APIs.
// Thresholds are configurable heuristics; content-wide qualification is pending.
[[nodiscard]] SceneAnalysis analyzePair(RgbView left, RgbView right, const SceneSettings& settings = {});
} // namespace rifelove::quality

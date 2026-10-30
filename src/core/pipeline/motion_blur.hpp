#pragma once

#include "common/rational.hpp"
#include "interpolation/scene_analysis.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace rifelove::pipeline {

// The SDR blend operates in linear light and converts back to the source
// transfer curve only after accumulation.
[[nodiscard]] float sdrToLinear(float value, std::string_view transfer);
[[nodiscard]] float linearToSdr(float value, std::string_view transfer);

// Returns exact temporal sample positions for one output frame. The output
// frame remains at center; the samples are only used to synthesize its
// exposure. A zero angle or one sample is intentionally left to the caller as
// a disabled/legacy path.
[[nodiscard]] std::vector<Rational> motionBlurSampleTimes(
    Rational center, Rational targetRate, int shutterAngleDegrees, int sampleCount);

struct MotionBlurSampleLocation {
    // For an exact source-frame or edge hold, sourceIndex is valid and
    // sourceFrame is true. For interpolation, leftIndex/rightIndex and the
    // exact rational timestep are valid. A non-interpolable pair is reported
    // as blocked so callers can fall back to the center sample without mixing
    // across a quality barrier.
    std::size_t sourceIndex = 0;
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;
    Rational timestep{0};
    bool sourceFrame = false;
    bool blocked = false;
};

// Resolves one sample against a bounded source-frame window. The pairActions
// span has one fewer element than sourceTimestamps and describes the interval
// between each adjacent pair. Source timestamps must be strictly increasing.
// Samples outside the decoded range use edge frames; samples inside a pair
// whose action is not interpolate are blocked.
[[nodiscard]] MotionBlurSampleLocation resolveMotionBlurSample(
    std::span<const Rational> sourceTimestamps,
    std::span<const quality::PairAction> pairActions,
    Rational sampleTime);

// A temporal exposure must not straddle a blocked interval. This catches the
// case where uniformly spaced samples happen to skip over the interval itself.
[[nodiscard]] bool motionBlurWindowCrossesBlockedPair(
    std::span<const Rational> sourceTimestamps,
    std::span<const quality::PairAction> pairActions,
    Rational windowStart, Rational windowEnd);

} // namespace rifelove::pipeline

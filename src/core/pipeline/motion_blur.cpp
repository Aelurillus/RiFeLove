#include "core/pipeline/motion_blur.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rifelove::pipeline {

float sdrToLinear(float value, std::string_view transfer) {
    value = std::clamp(value,0.0F,1.0F);
    if (transfer == "iec61966-2-1")
        return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F,2.4F);
    // BT.709, SMPTE 170M and the validated SDR profiles use the same
    // piecewise OETF form for this blend.
    return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F,1.0F / 0.45F);
}

float linearToSdr(float value, std::string_view transfer) {
    value = std::max(0.0F,value);
    if (transfer == "iec61966-2-1")
        return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value,1.0F / 2.4F) - 0.055F;
    return value < 0.018F ? value * 4.5F : 1.099F * std::pow(value,0.45F) - 0.099F;
}

std::vector<Rational> motionBlurSampleTimes(
    Rational center, Rational targetRate, int shutterAngleDegrees, int sampleCount) {
    if (targetRate <= Rational(0)) throw std::invalid_argument("Motion blur target rate must be positive");
    if (shutterAngleDegrees < 0 || shutterAngleDegrees > 360)
        throw std::invalid_argument("Motion blur shutter angle must be 0..360 degrees");
    if (sampleCount < 1 || sampleCount > 16)
        throw std::invalid_argument("Motion blur sample count must be 1..16");
    const auto exposure = Rational(shutterAngleDegrees,360) / targetRate;
    std::vector<Rational> result;
    result.reserve(static_cast<std::size_t>(sampleCount));
    for (int index = 0; index < sampleCount; ++index) {
        const auto offsetNumerator = 2 * index + 1 - sampleCount;
        result.push_back(center + exposure * Rational(offsetNumerator,2 * sampleCount));
    }
    return result;
}

std::pair<Rational,Rational> motionBlurExposureBounds(
    Rational center, Rational targetRate, int shutterAngleDegrees) {
    if (targetRate <= Rational(0)) throw std::invalid_argument("Motion blur target rate must be positive");
    if (shutterAngleDegrees < 0 || shutterAngleDegrees > 360)
        throw std::invalid_argument("Motion blur shutter angle must be 0..360 degrees");
    const auto halfExposure = Rational(shutterAngleDegrees,720) / targetRate;
    return {center - halfExposure,center + halfExposure};
}

MotionBlurSampleLocation resolveMotionBlurSample(
    std::span<const Rational> sourceTimestamps,
    std::span<const quality::PairAction> pairActions,
    Rational sampleTime) {
    if (sourceTimestamps.empty()) throw std::invalid_argument("Motion blur sample resolution needs source timestamps");
    if (pairActions.size() + 1 != sourceTimestamps.size())
        throw std::invalid_argument("Motion blur pair actions do not match source timestamps");
    for (std::size_t index = 1; index < sourceTimestamps.size(); ++index)
        if (sourceTimestamps[index] <= sourceTimestamps[index - 1])
            throw std::invalid_argument("Motion blur source timestamps must be strictly increasing");
    if (sampleTime <= sourceTimestamps.front()) return {0,0,0,Rational(0),true,false};
    if (sampleTime >= sourceTimestamps.back()) {
        const auto last = sourceTimestamps.size() - 1;
        return {last,last,last,Rational(0),true,false};
    }
    const auto after = std::upper_bound(sourceTimestamps.begin(),sourceTimestamps.end(),sampleTime);
    const auto right = static_cast<std::size_t>(std::distance(sourceTimestamps.begin(),after));
    const auto left = right - 1;
    if (sourceTimestamps[left] == sampleTime) return {left,left,left,Rational(0),true,false};
    if (pairActions[left] != quality::PairAction::interpolate)
        return {0,left,right,Rational(0),false,true};
    const auto span = sourceTimestamps[right] - sourceTimestamps[left];
    return {0,left,right,(sampleTime - sourceTimestamps[left]) / span,false,false};
}

bool motionBlurWindowCrossesBlockedPair(
    std::span<const Rational> sourceTimestamps,
    std::span<const quality::PairAction> pairActions,
    Rational windowStart, Rational windowEnd) {
    if (sourceTimestamps.empty() || pairActions.size() + 1 != sourceTimestamps.size())
        throw std::invalid_argument("Motion blur barrier inputs do not match");
    if (windowEnd < windowStart) std::swap(windowStart,windowEnd);
    for (std::size_t index = 0; index < pairActions.size(); ++index) {
        if (pairActions[index] == quality::PairAction::interpolate) continue;
        // Touching a source-frame boundary is safe. A window must enter the
        // blocked interval before it is considered a cross-barrier exposure.
        if (windowStart < sourceTimestamps[index + 1] && windowEnd > sourceTimestamps[index]) return true;
    }
    return false;
}

MotionBlurExposurePlan planMotionBlurExposure(
    std::span<const Rational> sourceTimestamps,
    std::span<const quality::PairAction> pairActions,
    Rational center, Rational targetRate, int shutterAngleDegrees, int sampleCount,
    bool beginsAtSourceStart, bool endOfSource) {
    const auto sampleTimes = motionBlurSampleTimes(center,targetRate,shutterAngleDegrees,sampleCount);
    const auto [windowStart,windowEnd] = motionBlurExposureBounds(center,targetRate,shutterAngleDegrees);
    if (sourceTimestamps.empty() || pairActions.size() + 1 != sourceTimestamps.size())
        throw std::invalid_argument("Motion blur exposure needs a matching source window");
    // Resolve once to validate strictly increasing timestamps, even while
    // waiting for lookahead. Losing history is an error, not a blur fallback.
    MotionBlurExposurePlan plan;
    plan.center = resolveMotionBlurSample(sourceTimestamps,pairActions,center);
    if (!beginsAtSourceStart && windowStart < sourceTimestamps.front())
        throw std::logic_error("Motion blur source history does not cover the full shutter");
    if (!endOfSource && windowEnd > sourceTimestamps.back()) return plan;
    plan.ready = true;
    plan.edgeHeld = windowStart < sourceTimestamps.front() || windowEnd > sourceTimestamps.back();
    if (plan.center.blocked) {
        plan.fallbackReason = std::string(quality::toString(pairActions[plan.center.leftIndex]));
        return plan;
    }
    for (std::size_t index = 0; index < pairActions.size(); ++index) {
        if (pairActions[index] != quality::PairAction::interpolate &&
            windowStart < sourceTimestamps[index + 1] && windowEnd > sourceTimestamps[index]) {
            plan.fallbackReason = std::string(quality::toString(pairActions[index]));
            return plan;
        }
    }
    plan.samples.reserve(sampleTimes.size());
    for (const auto time : sampleTimes) {
        auto location = resolveMotionBlurSample(sourceTimestamps,pairActions,time);
        if (location.blocked) throw std::logic_error("Motion blur sample crossed an unchecked quality barrier");
        plan.edgeHeld = plan.edgeHeld || time < sourceTimestamps.front() || time > sourceTimestamps.back();
        plan.samples.push_back(location);
    }
    return plan;
}

std::size_t motionBlurDiscardableFrames(
    std::span<const Rational> sourceTimestamps, Rational nextWindowStart) {
    if (sourceTimestamps.empty()) return 0;
    const auto after = std::upper_bound(sourceTimestamps.begin(),sourceTimestamps.end(),nextWindowStart);
    // The last timestamp <= the next shutter start is its left bracket.
    return after == sourceTimestamps.begin() ? 0 :
        static_cast<std::size_t>(std::distance(sourceTimestamps.begin(),after) - 1);
}

} // namespace rifelove::pipeline

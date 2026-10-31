#include "restoration/dynamic_control.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rifelove::restoration {
namespace {
bool unit(double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; }
}

DynamicController::DynamicController(DynamicSettings settings, std::string identity) : settings_(settings) {
    if (identity.empty() || identity.size() > 4096 || !std::isfinite(settings.smoothingSeconds) || settings.smoothingSeconds <= 0.0 ||
        !std::isfinite(settings.maximumChangePerSecond) || settings.maximumChangePerSecond <= 0.0 || !unit(settings.minimumConfidence))
        throw std::invalid_argument("Invalid dynamic-restoration control identity or smoothing settings");
    for (const auto& effect : settings.effects)
        if (!std::isfinite(effect.relative) || effect.relative < -1.0 || effect.relative > 1.0)
            throw std::invalid_argument("Dynamic restoration relative adjustments must be in [-1,1]");
    state_.identity = std::move(identity);
}

double DynamicController::relativeStrength(double automatic, double relative) {
    if (!unit(automatic) || !std::isfinite(relative) || relative < -1.0 || relative > 1.0)
        throw std::invalid_argument("Invalid dynamic restoration strength");
    return automatic + relative * (relative < 0.0 ? automatic : 1.0 - automatic);
}

DynamicValues DynamicController::update(const DynamicObservation& observation) {
    for (const auto& effect : observation.effects)
        if (!unit(effect.automatic) || !unit(effect.confidence))
            throw std::invalid_argument("Invalid dynamic restoration estimate or confidence");
    if (state_.lastTimestamp && observation.timestamp <= *state_.lastTimestamp)
        throw std::invalid_argument("Dynamic restoration requires strictly increasing presentation timestamps");
    // Compute the complete next state before publishing it, including rational
    // arithmetic which can throw for an unrepresentable time difference.
    auto next = state_;
    const double delta = state_.lastTimestamp ? static_cast<double>((observation.timestamp - *state_.lastTimestamp).toLongDouble()) : 0.0;
    if (observation.sceneCut) {
        if (next.sceneSerial == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Dynamic restoration scene counter overflow");
        ++next.sceneSerial;
        next.automatic = {};
        next.hasEstimate = {};
    }
    const double alpha = -std::expm1(-delta / settings_.smoothingSeconds);
    DynamicValues result;
    for (std::size_t index = 0; index < next.automatic.size(); ++index) {
        const auto& observed = observation.effects[index];
        // Zero is the sentinel for missing evidence, even when the caller
        // explicitly chooses a minimum threshold of zero. In particular,
        // bypassed frames must not silently update the smoothing state.
        if (observed.confidence > 0.0 && observed.confidence >= settings_.minimumConfidence) {
            if (!next.hasEstimate[index]) next.automatic[index] = observed.automatic;
            else {
                const double change = alpha * (observed.automatic - next.automatic[index]);
                const double limit = settings_.maximumChangePerSecond * delta;
                next.automatic[index] += std::clamp(change,-limit,limit);
            }
            next.hasEstimate[index] = true;
        }
        result.automatic[index] = next.automatic[index];
        result.hasEstimate[index] = next.hasEstimate[index];
        // Relative controls cannot fabricate an estimate. Adapters must still
        // bypass their current frame when its current evidence is uncertain;
        // hasEstimate records smoothing history, not current confidence.
        result.strength[index] = settings_.effects[index].enabled && next.hasEstimate[index]
            ? relativeStrength(next.automatic[index],settings_.effects[index].relative) : 0.0;
    }
    next.lastTimestamp = observation.timestamp;
    state_ = std::move(next);
    return result;
}

void DynamicController::restore(const DynamicCheckpoint& checkpoint) {
    if (checkpoint.version != 1 || checkpoint.identity != state_.identity)
        throw std::invalid_argument("Dynamic restoration checkpoint does not match the model/analyzer/settings identity");
    for (std::size_t index = 0; index < checkpoint.automatic.size(); ++index)
        if (!unit(checkpoint.automatic[index]) || (!checkpoint.hasEstimate[index] && checkpoint.automatic[index] != 0.0) ||
            (!checkpoint.lastTimestamp && checkpoint.hasEstimate[index]))
            throw std::invalid_argument("Dynamic restoration checkpoint has invalid state");
    state_ = checkpoint;
}

} // namespace rifelove::restoration

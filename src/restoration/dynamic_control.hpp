#pragma once

#include "common/rational.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace rifelove::restoration {

// Independent user controls. Zero means the estimated automatic value, -1
// disables an effect and +1 requests its maximum supported strength. This is
// an interaction contract, not a claim to reproduce any proprietary algorithm.
enum class DynamicEffect : std::size_t { compression = 0, detail = 1, denoise = 2 };
struct DynamicAdjustment { bool enabled = true; double relative = 0.0; };
struct DynamicSettings {
    std::array<DynamicAdjustment,3> effects{};
    double smoothingSeconds = 0.35;
    double maximumChangePerSecond = 0.5;
    double minimumConfidence = 0.5;
};
// Zero confidence denotes missing evidence even with minimumConfidence == 0.
struct DynamicEstimate { double automatic = 0.0; double confidence = 0.0; };
struct DynamicObservation {
    Rational timestamp;
    std::array<DynamicEstimate,3> effects{};
    bool sceneCut = false;
};
struct DynamicValues {
    std::array<double,3> automatic{};
    std::array<double,3> strength{};
    std::array<bool,3> hasEstimate{};
};
// Save after the last committed output frame, together with the model/weight,
// analyzer/settings identity and that frame's PTS. A resumed pipeline must
// reconstruct the same scene-bounded decoded window before advancing it.
struct DynamicCheckpoint {
    int version = 1;
    std::string identity;
    std::optional<Rational> lastTimestamp;
    std::array<double,3> automatic{};
    std::array<bool,3> hasEstimate{};
    std::uint64_t sceneSerial = 0;
};

class DynamicController final {
public:
    explicit DynamicController(DynamicSettings settings, std::string identity);
    [[nodiscard]] DynamicValues update(const DynamicObservation& observation);
    [[nodiscard]] DynamicCheckpoint checkpoint() const { return state_; }
    void restore(const DynamicCheckpoint& checkpoint);
    [[nodiscard]] static double relativeStrength(double automatic, double relative);
private:
    DynamicSettings settings_;
    DynamicCheckpoint state_;
};

} // namespace rifelove::restoration

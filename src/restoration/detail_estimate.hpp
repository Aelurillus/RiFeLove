#pragma once

#include "media/rife_color.hpp"
#include "restoration/dynamic_control.hpp"

#include <cstddef>
#include <stop_token>

namespace rifelove::restoration {

inline constexpr int automaticDetailAlgorithmVersion = 2;
inline constexpr const char* automaticDetailAnalyzerId = "sdr8-directional-soft-edge-v2";
inline constexpr float maximumAutomaticDeblurBlend = 0.35F;

struct DetailEstimate {
    double strength = 0.0; // Unit interval before the conservative blend cap.
    double confidence = 0.0;
    std::size_t sampledPositions = 0;
};

// Looks for numerous, spatially distributed soft step edges concentrated in
// one direction, with sharp edges on the other axis as an image reference.
// This is evidence of directional softness, not proof of motion blur:
// naturally soft textures or intentional defocus may also trigger the model.
// This experimental estimate must not be used as a default without realistic
// false-positive and temporal-quality evaluation.
// Input is the unpadded, finite nonlinear SDR RGB tensor from prepareRifeSdr.
[[nodiscard]] DetailEstimate estimateDirectionalSoftEdges(
    const media::RifeSdrFrame& frame, std::stop_token stop = {});

struct AutomaticDetailSettings {
    double relativeAdjustment = 0.0;
    double smoothingSeconds = 0.35;
    double maximumStrengthChangePerSecond = 0.5;
    double minimumConfidence = 0.6;
};

struct AutomaticDetailValue {
    float deblurStrength = 0.0F;
    bool confident = false;
};

// Advance on strictly increasing exact source PTS. Replaying the original
// decoded source from the start reconstructs the same scene-bounded state for
// checkpoint resume; a low-confidence current frame always returns zero.
class AutomaticDetailController final {
public:
    explicit AutomaticDetailController(AutomaticDetailSettings settings);
    [[nodiscard]] AutomaticDetailValue update(
        Rational timestamp, const DetailEstimate& estimate, bool sceneCut = false);
private:
    AutomaticDetailSettings settings_;
    DynamicController control_;
};

} // namespace rifelove::restoration

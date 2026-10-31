#pragma once

#include "media/media_engine.hpp"
#include "restoration/dynamic_control.hpp"

#include <cstddef>
#include <stop_token>

namespace rifelove::restoration {

inline constexpr int automaticCompressionAlgorithmVersion = 2;
inline constexpr const char* automaticCompressionReplayPolicy = "software-source-pts-from-start-v1";
inline constexpr const char* automaticCompressionAnalyzerId = "sdr8-luma-block8-stratified-v2";

struct CompressionEstimate {
    double strength = 0.0;
    double confidence = 0.0;
    std::size_t samples = 0;
};

// Bounded, conservative evidence for distributed 8-pixel luma discontinuities.
// It is not a codec/QP detector or a detail-recovery model. Texture, clipping,
// insufficient support and one-direction-only edges are intentionally rejected.
// Uses only the original, extent-proven BT.709 TV-range YUV420P source plane.
// Deterministic two-dimensional strata cover every 8-pixel seam with at most
// 8192 samples per axis; no shared fixed sampling stride is used.
[[nodiscard]] CompressionEstimate estimateCompressionArtifacts(
    const media::DecodedFrame& frame, std::stop_token stop = {});

struct AutomaticCompressionSettings {
    double relativeAdjustment = 0.0;
    double smoothingSeconds = 0.35;
    double maximumStrengthChangePerSecond = 0.5;
    double minimumConfidence = 0.6;
};

struct AutomaticCompressionValue {
    float strength = 0.0F;
    bool confident = false;
};

// Exact source-PTS smoothing, reset at cuts. An ambiguous current frame always
// bypasses STDF, even if a previous frame had a confident estimate. Replaying
// original frames from the beginning deterministically reconstructs this state.
class AutomaticCompressionController final {
public:
    explicit AutomaticCompressionController(AutomaticCompressionSettings settings);
    [[nodiscard]] AutomaticCompressionValue update(
        Rational timestamp, const CompressionEstimate& estimate, bool sceneCut = false);
private:
    AutomaticCompressionSettings settings_;
    DynamicController control_;
};

} // namespace rifelove::restoration

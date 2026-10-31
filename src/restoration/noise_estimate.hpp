#pragma once

#include "media/rife_color.hpp"
#include "restoration/dynamic_control.hpp"

#include <cstddef>
#include <stop_token>

namespace rifelove::restoration {

inline constexpr int automaticDenoiseAlgorithmVersion = 2;
inline constexpr const char* automaticDenoiseReplayPolicy = "software-source-pts-from-start-v1";

struct GaussianNoiseEstimate {
    double sigma255 = 0.0;
    double confidence = 0.0;
    std::size_t samplesPerChannel = 0;
};

// Robust, bounded spatial estimate for additive Gaussian noise in the actual
// (unpadded) nonlinear RGB image. It is not a texture/compression classifier.
// Insufficient, clipped or strongly non-Gaussian samples have low confidence.
[[nodiscard]] GaussianNoiseEstimate estimateGaussianNoise(
    const media::RifeSdrFrame& frame, std::stop_token stop = {},
    int algorithmVersion = automaticDenoiseAlgorithmVersion);

struct AutomaticDenoiseSettings {
    double relativeAdjustment = 0.0;
    double smoothingSeconds = 0.35;
    // Maximum change in sigma (8-bit levels) per source-video second.
    double maximumSigmaChangePerSecond = 27.5;
    double minimumConfidence = 0.6;
    // Persisted jobs retain v1; new jobs use the bounded stratified v2 sampler.
    int algorithmVersion = automaticDenoiseAlgorithmVersion;
};

struct AutomaticDenoiseValue {
    float sigma255 = 0.0F; // Exactly zero bypasses the model; otherwise 5..55.
    bool confident = false;
};

// Source-PTS smoothing with a fresh state at every scene boundary. Ambiguous
// content bypasses denoising rather than borrowing a trusted-looking estimate
// from a different image. Values below the adapter's trained minimum bypass.
class AutomaticDenoiseController final {
public:
    explicit AutomaticDenoiseController(AutomaticDenoiseSettings settings);
    [[nodiscard]] AutomaticDenoiseValue update(
        Rational timestamp, const GaussianNoiseEstimate& estimate, bool sceneCut = false);
private:
    AutomaticDenoiseSettings settings_;
    DynamicController control_;
};

} // namespace rifelove::restoration

#include "restoration/noise_estimate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rifelove::restoration {
namespace {
constexpr double gaussianMedianAbsolute = 0.6744897501960817;
constexpr std::size_t maximumSamples = 16384;

void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Gaussian-noise analysis cancelled");
}

double quantile(std::vector<float>& values, std::size_t index) {
    std::nth_element(values.begin(),values.begin() + static_cast<std::ptrdiff_t>(index),values.end());
    return values[index];
}

// A stable mixed offset per stratum prevents the fixed raster stride from
// sampling the same phase of a periodic texture in every row. It is independent
// of frame number so static content does not flicker due to the sampler.
std::uint64_t stratumHash(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

DynamicSettings dynamicSettings(const AutomaticDenoiseSettings& settings) {
    if (!std::isfinite(settings.relativeAdjustment) || settings.relativeAdjustment < -1.0 || settings.relativeAdjustment > 1.0 ||
        !std::isfinite(settings.maximumSigmaChangePerSecond) || settings.maximumSigmaChangePerSecond <= 0.0 ||
        !std::isfinite(settings.minimumConfidence) || settings.minimumConfidence < 0.0 || settings.minimumConfidence > 1.0 ||
        (settings.algorithmVersion != 1 && settings.algorithmVersion != 2))
        throw std::invalid_argument("Invalid automatic Gaussian-denoise settings");
    DynamicSettings result;
    result.effects[0].enabled = false;
    result.effects[1].enabled = false;
    result.effects[2].relative = settings.relativeAdjustment;
    result.smoothingSeconds = settings.smoothingSeconds;
    result.maximumChangePerSecond = settings.maximumSigmaChangePerSecond / 55.0;
    // Even an explicitly zero threshold cannot make an unavailable estimate
    // (confidence zero) become automatic model conditioning.
    result.minimumConfidence = std::max(settings.minimumConfidence,std::numeric_limits<double>::epsilon());
    return result;
}
}

GaussianNoiseEstimate estimateGaussianNoise(const media::RifeSdrFrame& frame, std::stop_token stop, int algorithmVersion) {
    cancelled(stop);
    if (algorithmVersion != 1 && algorithmVersion != 2)
        throw std::invalid_argument("Unsupported Gaussian-noise analyzer version");
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
        frame.paddedWidth < frame.width || frame.paddedHeight < frame.height || frame.paddedWidth > 8192 || frame.paddedHeight > 8192)
        throw std::invalid_argument("Invalid Gaussian-noise analysis dimensions");
    const auto plane = static_cast<std::size_t>(frame.paddedWidth) * static_cast<std::size_t>(frame.paddedHeight);
    if (frame.pixels.size() != plane * 3)
        throw std::invalid_argument("Invalid Gaussian-noise analysis tensor size");
    if (frame.width < 3 || frame.height < 3) return {};
    // The separable [1,-2,1] x [1,-2,1] kernel kills constant/linear image
    // gradients. Its squared coefficient sum is 36, hence division by six
    // leaves an IID Gaussian standard deviation unchanged. Non-overlapping
    // 3x3 supports avoid manufacturing confidence from correlated samples.
    const int columns = frame.width / 3, rows = frame.height / 3;
    const auto candidates = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    const auto stride = std::max<std::size_t>(1,(candidates + maximumSamples - 1) / maximumSamples);
    const auto sampleCount = algorithmVersion == 1 ? (candidates + stride - 1) / stride : std::min(candidates,maximumSamples);
    std::array<std::vector<float>,3> residuals;
    for (auto& channel : residuals) channel.reserve(std::min(candidates,maximumSamples));
    std::size_t examined = 0;
    for (std::size_t stratum = 0; stratum < sampleCount; ++stratum) {
        if ((examined++ & 255U) == 0) cancelled(stop);
        const auto begin = stratum * candidates / sampleCount;
        const auto end = (stratum + 1) * candidates / sampleCount;
        const auto sample = algorithmVersion == 1 ? stratum * stride : begin + stratumHash(stratum) % (end - begin);
        const auto x = static_cast<int>(sample % static_cast<std::size_t>(columns)) * 3;
        const auto y = static_cast<int>(sample / static_cast<std::size_t>(columns)) * 3;
        std::array<float,3> coefficients{};
        bool clipped = false;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            double coefficient = 0.0;
            constexpr int kernel[3] = {1,-2,1};
            for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 3; ++dx) {
                const auto offset = channel * plane + static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(frame.paddedWidth) + static_cast<std::size_t>(x + dx);
                const double value = frame.pixels[offset];
                if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                    throw std::invalid_argument("Gaussian-noise analysis requires finite normalized RGB pixels");
                clipped = clipped || value <= 0.005 || value >= 0.995;
                coefficient += value * kernel[dx] * kernel[dy];
            }
            coefficients[channel] = static_cast<float>(std::abs(coefficient) * (255.0 / 6.0));
        }
        // Saturated supports do not follow the additive Gaussian assumption.
        // Keep the same valid supports for all three channel estimates.
        if (!clipped) for (std::size_t channel = 0; channel < 3; ++channel) residuals[channel].push_back(coefficients[channel]);
    }
    cancelled(stop);
    const auto count = residuals[0].size();
    if (count < 64) return {0.0,0.0,count};
    std::array<double,3> sigma{}, shape{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        auto& values = residuals[channel];
        const double median = quantile(values,count / 2);
        const double upperQuartile = quantile(values,(count * 3) / 4);
        sigma[channel] = median / gaussianMedianAbsolute;
        // Clean smooth regions are a valid zero-noise observation. A nearly
        // constant high-frequency pattern is not Gaussian noise; its ratio is
        // near one rather than the Gaussian ~1.705.
        shape[channel] = sigma[channel] < 0.25 ? 1.0 : std::clamp((upperQuartile / std::max(median,1e-12) - 1.1) / 0.4,0.0,1.0);
    }
    std::sort(sigma.begin(),sigma.end());
    const double estimate = sigma[1] < 0.25 ? 0.0 : sigma[1];
    const double coverage = static_cast<double>(count) / static_cast<double>(examined);
    const double confidence = std::min(1.0,static_cast<double>(count) / 1024.0) * coverage *
        *std::min_element(shape.begin(),shape.end());
    // The local model was conditioned on sigma 5..55. Out-of-domain content
    // must not silently turn the strongest available setting into a diagnosis.
    return {estimate,estimate > 55.0 ? 0.0 : confidence,count};
}

AutomaticDenoiseController::AutomaticDenoiseController(AutomaticDenoiseSettings settings)
    : settings_(settings),control_(dynamicSettings(settings),"gaussian-rgb-laplacian-mad-v" + std::to_string(settings.algorithmVersion)) {}

AutomaticDenoiseValue AutomaticDenoiseController::update(
    Rational timestamp, const GaussianNoiseEstimate& estimate, bool sceneCut) {
    if (!std::isfinite(estimate.sigma255) || estimate.sigma255 < 0.0 ||
        !std::isfinite(estimate.confidence) || estimate.confidence < 0.0 || estimate.confidence > 1.0)
        throw std::invalid_argument("Invalid Gaussian-noise estimate");
    const bool confident = estimate.confidence > 0.0 && estimate.confidence >= settings_.minimumConfidence && estimate.sigma255 <= 55.0;
    DynamicObservation observation{timestamp,{},sceneCut};
    observation.effects[2] = {std::clamp(estimate.sigma255 / 55.0,0.0,1.0),confident ? estimate.confidence : 0.0};
    const auto value = control_.update(observation);
    const auto sigma = value.strength[2] * 55.0;
    return {confident && sigma >= 5.0 ? static_cast<float>(std::min(sigma,55.0)) : 0.0F,confident};
}

} // namespace rifelove::restoration

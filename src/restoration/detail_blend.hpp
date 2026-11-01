#pragma once

#include <cmath>
#include <span>
#include <stdexcept>
#include <stop_token>

namespace rifelove::restoration {

// Blend a same-resolution model residual in the adapter's normalized nonlinear
// RGB domain, before output quantization. No sharpening or missing-detail
// inference is claimed. Callers supply equal-layout, non-overlapping tensors.
// Both inputs are checked before any output is changed, even at the endpoints.
inline void blendDeblurPixels(std::span<const float> source, std::span<float> restored,
    float strength, std::stop_token stop = {}) {
    const auto cancelled = [&] {
        if (stop.stop_requested()) throw std::runtime_error("Deblur residual blending cancelled");
    };
    cancelled();
    if (!std::isfinite(strength) || strength < 0.0F || strength > 1.0F ||
        source.empty() || source.size() != restored.size())
        throw std::invalid_argument("Deblur residual blend requires equal nonempty tensors and a finite strength in [0,1]");
    for (std::size_t index = 0; index < source.size(); ++index) {
        if ((index & 4095U) == 0) cancelled();
        if (!std::isfinite(source[index]) || source[index] < 0.0F || source[index] > 1.0F ||
            !std::isfinite(restored[index]) || restored[index] < 0.0F || restored[index] > 1.0F)
            throw std::invalid_argument("Deblur residual blend requires finite normalized RGB pixels");
    }
    if (strength == 1.0F) return;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if ((index & 4095U) == 0) cancelled();
        restored[index] = strength == 0.0F ? source[index]
            : (1.0F - strength) * source[index] + strength * restored[index];
    }
}

} // namespace rifelove::restoration

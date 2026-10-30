#pragma once

#include "common/rational.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace rifelove {

// Approximate output count from a declared source duration. Runtime scheduling
// still uses decoded PTS and the verified exclusive end boundary.
[[nodiscard]] std::optional<std::size_t> estimateOutputFrameCount(std::optional<Rational> sourceDuration, Rational targetRate);

// Streaming CFR clock: memory use is independent of clip length. Source pair
// selection remains with the decoder; each timestamp is derived from the exact
// origin and integer output index, never accumulated floating-point deltas.
class CfrClock final {
public:
    CfrClock(Rational origin, Rational rate);
    [[nodiscard]] Rational timestamp() const;
    [[nodiscard]] Rational period() const { return Rational(1) / rate_; }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    void advance();
private:
    Rational origin_;
    Rational rate_;
    std::size_t index_ = 0;
};

struct FrameSample {
    std::size_t outputIndex;
    Rational timestamp;
    std::size_t frame0;
    std::size_t frame1;
    Rational timestep;
    bool sourceFrame;
    bool terminalHold;
};

class Timeline final {
public:
    // PTS values are seconds relative to a chosen clip origin. endExclusive is mandatory.
    Timeline(std::vector<Rational> presentationTimes, Rational endExclusive, Rational targetRate);
    [[nodiscard]] std::size_t outputFrameCount() const noexcept { return outputCount_; }
    [[nodiscard]] Rational outputRate() const noexcept { return targetRate_; }
    [[nodiscard]] FrameSample sample(std::size_t outputIndex) const;

private:
    std::vector<Rational> pts_;
    Rational end_;
    Rational targetRate_;
    std::size_t outputCount_;
};

} // namespace rifelove

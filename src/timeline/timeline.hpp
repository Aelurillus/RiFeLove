#pragma once

#include "common/rational.hpp"

#include <cstddef>
#include <vector>

namespace rifelove {

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

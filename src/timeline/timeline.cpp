#include "timeline/timeline.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rifelove {

Timeline::Timeline(std::vector<Rational> presentationTimes, Rational endExclusive, Rational targetRate)
    : pts_(std::move(presentationTimes)), end_(endExclusive), targetRate_(targetRate), outputCount_(0) {
    if (pts_.empty()) throw std::invalid_argument("timeline needs at least one input frame");
    if (targetRate_ <= Rational(0)) throw std::invalid_argument("target frame rate must be positive");
    for (std::size_t i = 1; i < pts_.size(); ++i)
        if (pts_[i] <= pts_[i - 1]) throw std::invalid_argument("input PTS values must be strictly increasing");
    if (end_ <= pts_.back()) throw std::invalid_argument("exclusive end must be later than the last frame PTS");

    const auto exactCount = (end_ - pts_.front()) * targetRate_;
    auto whole = exactCount.numerator() / exactCount.denominator();
    if (exactCount.numerator() % exactCount.denominator() > 0) ++whole;
    if (whole <= 0 || static_cast<std::uint64_t>(whole) > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        throw std::overflow_error("output frame count is out of range");
    outputCount_ = static_cast<std::size_t>(whole);
}

FrameSample Timeline::sample(std::size_t outputIndex) const {
    if (outputIndex >= outputCount_) throw std::out_of_range("output frame index is outside the timeline");
    const auto timestamp = pts_.front() + Rational(static_cast<std::int64_t>(outputIndex)) / targetRate_;
    const auto after = std::upper_bound(pts_.begin(), pts_.end(), timestamp);
    if (after == pts_.begin()) return {outputIndex, timestamp, 0, 0, Rational(0), true, false};
    const auto left = static_cast<std::size_t>(std::distance(pts_.begin(), after) - 1);
    if (pts_[left] == timestamp) return {outputIndex, timestamp, left, left, Rational(0), true, false};
    if (left + 1 == pts_.size()) return {outputIndex, timestamp, left, left, Rational(0), false, true};
    const auto span = pts_[left + 1] - pts_[left];
    const auto step = (timestamp - pts_[left]) / span;
    return {outputIndex, timestamp, left, left + 1, step, false, false};
}

} // namespace rifelove

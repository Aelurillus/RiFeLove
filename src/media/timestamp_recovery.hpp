#pragma once

#include "common/rational.hpp"
#include <optional>

namespace rifelove::media {

struct TimestampResolution {
    std::optional<Rational> timestamp;
    bool recovered = false;
};

// Repairs only timestamps supported by explicit stream-start or prior-frame
// duration evidence. It deliberately does not infer VFR timing from average FPS.
class TimestampRecovery final {
public:
    explicit TimestampRecovery(std::optional<Rational> declaredStart = {}) : declaredStart_(declaredStart) {}

    [[nodiscard]] TimestampResolution resolve(std::optional<Rational> observed,
        std::optional<Rational> duration) {
        if (observed) return remember(*observed,duration,false);
        if (!seenFrame_ && declaredStart_) return remember(*declaredStart_,duration,true);
        if (lastTimestamp_ && lastDuration_ && *lastDuration_ > Rational(0))
            return remember(*lastTimestamp_ + *lastDuration_,duration,true);
        seenFrame_ = true;
        lastTimestamp_.reset();
        lastDuration_ = positive(duration);
        return {};
    }

private:
    [[nodiscard]] static std::optional<Rational> positive(std::optional<Rational> value) {
        if (value && *value <= Rational(0)) return {};
        return value;
    }
    [[nodiscard]] TimestampResolution remember(Rational timestamp, std::optional<Rational> duration, bool recovered) {
        seenFrame_ = true;
        lastTimestamp_ = timestamp;
        lastDuration_ = positive(duration);
        return {timestamp,recovered};
    }

    std::optional<Rational> declaredStart_;
    std::optional<Rational> lastTimestamp_;
    std::optional<Rational> lastDuration_;
    bool seenFrame_ = false;
};

} // namespace rifelove::media

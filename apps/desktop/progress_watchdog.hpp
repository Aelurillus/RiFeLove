#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace rifelove::desktop {

class ProgressWatchdog final {
public:
    using Clock = std::chrono::steady_clock;

    void reset() noexcept {
        lastFrame_.reset();
        lastProgress_.reset();
    }

    [[nodiscard]] bool observe(std::uint64_t outputFrames, Clock::time_point now = Clock::now()) noexcept {
        if (!lastFrame_ || outputFrames > *lastFrame_) {
            lastFrame_ = outputFrames;
            lastProgress_ = now;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool stalled(Clock::time_point now, Clock::duration timeout) const noexcept {
        return lastProgress_ && now - *lastProgress_ >= timeout;
    }

private:
    std::optional<std::uint64_t> lastFrame_;
    std::optional<Clock::time_point> lastProgress_;
};

} // namespace rifelove::desktop

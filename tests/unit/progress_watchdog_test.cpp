#include "progress_watchdog.hpp"

#include <gtest/gtest.h>

TEST(ProgressWatchdog, WarnsOnlyAfterFramesStopAndClearsWhenTheyAdvance) {
    using Clock = rifelove::desktop::ProgressWatchdog::Clock;
    const auto start = Clock::time_point{};
    rifelove::desktop::ProgressWatchdog watchdog;
    constexpr auto timeout = std::chrono::seconds(60);

    EXPECT_FALSE(watchdog.stalled(start + timeout * 2,timeout));
    EXPECT_TRUE(watchdog.observe(1,start));
    EXPECT_FALSE(watchdog.stalled(start + timeout - std::chrono::seconds(1),timeout));
    EXPECT_TRUE(watchdog.stalled(start + timeout,timeout));
    EXPECT_FALSE(watchdog.observe(1,start + timeout));
    EXPECT_TRUE(watchdog.stalled(start + timeout + std::chrono::seconds(1),timeout));
    EXPECT_TRUE(watchdog.observe(2,start + timeout + std::chrono::seconds(1)));
    EXPECT_FALSE(watchdog.stalled(start + timeout + std::chrono::seconds(2),timeout));
}

TEST(ProgressWatchdog, ResetStartsANewFrameSequence) {
    using Clock = rifelove::desktop::ProgressWatchdog::Clock;
    const auto start = Clock::time_point{};
    rifelove::desktop::ProgressWatchdog watchdog;
    (void)watchdog.observe(100,start);
    watchdog.reset();

    EXPECT_FALSE(watchdog.stalled(start + std::chrono::hours(1),std::chrono::seconds(1)));
    EXPECT_TRUE(watchdog.observe(1,start + std::chrono::hours(1)));
    EXPECT_FALSE(watchdog.stalled(start + std::chrono::hours(1) + std::chrono::milliseconds(999),std::chrono::seconds(1)));
}

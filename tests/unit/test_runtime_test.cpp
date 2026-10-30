#include <gtest/gtest.h>
#include <cstdlib>

// Exercise the test runner's immediate-exit path on both the MSVCRT desktop
// toolchain and the separate UCRT core toolchain. No product process exits.
TEST(TestRuntime, ImmediateExitReturnsRequestedStatus) {
    EXPECT_EXIT(std::_Exit(23), ::testing::ExitedWithCode(23), "");
}

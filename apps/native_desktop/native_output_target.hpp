#pragma once

#include "common/output_target.hpp"

namespace rifelove::native_desktop {
using NativeOutputTarget = OutputTarget;
[[nodiscard]] inline NativeOutputTarget nativeOutputTarget(int width, int height, int selection) {
    return calculateOutputTarget(width, height, selection);
}
} // namespace rifelove::native_desktop

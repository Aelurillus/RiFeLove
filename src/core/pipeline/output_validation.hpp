#pragma once

#include "core/pipeline/video_pipeline.hpp"

namespace rifelove::detail {
// Shared by publication and regression tests. Verifies the actual local file,
// including decoded timestamps, frame count, end boundary and copied streams.
// A successful metadata probe alone is never sufficient for publication.
[[nodiscard]] OutputTimingValidation validatePipelineOutput(const std::string& path, int selected,
    int expectedWidth, int expectedHeight, Rational origin, Rational end, Rational rate,
    std::size_t expectedCount, const media::MediaInfo& source,
    const std::function<void(const PipelineProgress&)>& progress = {}, std::stop_token stop = {});
} // namespace rifelove::detail

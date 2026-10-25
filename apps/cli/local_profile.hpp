#pragma once

#include "core/pipeline/video_pipeline.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rifelove::cli {

struct LocalProfile final {
    std::string name;
    Rational targetRate{60};
    std::filesystem::path modelDirectory;
    std::filesystem::path runtimeDirectory;
    std::filesystem::path outputLocation;
    std::string container;
    std::string qualityMode;
    int bitrateKbps = 8000;
    int quality = 32;
    int gopSize = 120;
    media::VideoEncodingSettings encoding;
};

[[nodiscard]] LocalProfile loadLocalProfile(const std::filesystem::path& file);
void applyLocalProfile(const LocalProfile& profile, PipelineSettings& settings,
    std::filesystem::path& modelDirectory, std::filesystem::path& runtimeDirectory);
void overrideEncoder(PipelineSettings& settings, const std::string& encoder);
void overrideBitrate(PipelineSettings& settings, std::int64_t bitRate);
void validateLocalProfileOutput(const LocalProfile& profile, const std::filesystem::path& output);
[[nodiscard]] std::string serializeLocalProfileSummary(const LocalProfile& profile);
int runLocalProfileCheckCommand(const std::vector<std::string>& args);

} // namespace rifelove::cli

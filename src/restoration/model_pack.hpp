#pragma once

#include "restoration/video_restoration_backend.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace rifelove::restoration {

// Local, declarative restoration pack. The selected adapter remains
// application-owned code; the pack only names it and supplies an ONNX graph.
struct ModelPack {
    std::filesystem::path directory;
    std::filesystem::path graphPath;
    std::string id;
    std::string displayName;
    std::string version;
    std::string graphSha256;
    Capabilities capabilities;
    std::string licenseId;
    std::string licenseSha256;
    std::string numericalProfile;
    std::vector<std::string> validatedBackends;
    std::string sourceRepository;
    std::string upstreamCommit;
    std::string licenseScopeEvidence;
    std::string sourceManifestSha256;
    std::string validationSha256;
    // Sidecar hashes prove local consistency only, not publisher identity or
    // perceptual quality.
    bool metadataIntegrityVerified = false;
};

[[nodiscard]] ModelPack loadModelPack(const std::filesystem::path& directory);

} // namespace rifelove::restoration

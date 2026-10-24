#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rifelove::inference {

// A descriptor is not an authenticity signature. Trusted catalog identity and
// custom-model consent belong to the model manager, above this integrity layer.
struct ModelPack {
    std::filesystem::path directory;
    std::string id;
    std::string version;
    std::string sha256;
    int spatialMultiple = 0;
    std::vector<std::byte> graph;
    std::string displayName;
    std::string family;
    int schemaVersion = 1;
    std::string licenseId;
    std::string licenseSha256;
    std::string numericalProfile;
    std::vector<std::string> validatedBackends;
    std::vector<std::string> supportedInputProfiles;
    bool hdrQualified = false;
    std::string sourceRepository;
    std::string upstreamCommit;
    std::string licenseScopeEvidence;
    std::string sourceManifestSha256;
    std::string validationSha256;
    // True means declared sidecar hashes match their files. It does not
    // authenticate the author or certify perceptual quality.
    bool metadataIntegrityVerified = false;
};

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> readLocalFile(const std::filesystem::path& path, std::size_t limit);
[[nodiscard]] std::vector<std::byte> readVerifiedFile(const std::filesystem::path& path, const std::string& expectedSha256, std::size_t limit);
[[nodiscard]] std::filesystem::path packFile(const std::filesystem::path& directory, const std::string& relative);
[[nodiscard]] ModelPack loadModelPack(const std::filesystem::path& directory);

} // namespace rifelove::inference

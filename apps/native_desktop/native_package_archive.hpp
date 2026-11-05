#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <vector>

namespace rifelove::native_desktop {

struct NativePackageArchiveFile final {
    std::string path;
    std::uint64_t sizeBytes = 0;
    std::string sha256;
};

struct NativePackageArchivePlan final {
    std::string rootDirectory;
    std::uint64_t archiveSizeBytes = 0;
    std::string archiveSha256;
    std::vector<NativePackageArchiveFile> files;
    std::uint64_t maximumUncompressedBytes = 512ULL * 1024 * 1024;
};

struct NativePackageArchiveProgress final {
    std::string phase;
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
};

class NativePackageInstallCancelled final : public std::runtime_error {
public:
    NativePackageInstallCancelled() : std::runtime_error("Model package installation was cancelled") {}
};

// Extracts only the exact README and model files declared by a validated
// distribution manifest. The caller verifies the archive SHA-256 before this
// function and each extracted file SHA-256 afterward.
[[nodiscard]] std::filesystem::path extractNativeModelPackageArchive(
    const std::filesystem::path& archive,
    const NativePackageArchivePlan& plan,
    const std::filesystem::path& stagingDirectory,
    std::stop_token stopToken = {},
    std::function<void(NativePackageArchiveProgress)> progress = {});

} // namespace rifelove::native_desktop

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace rifelove::native_desktop {

struct NativeModelDownloadProgress final {
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
};

// Downloads only HTTPS resources. A validated expected size and digest bind a
// resumable partial file to the same model artifact across restarts.
void downloadNativeModelFile(
    const std::wstring& url,
    const std::filesystem::path& destination,
    std::uint64_t expectedSizeBytes,
    const std::string& expectedSha256,
    std::uint64_t maximumBytes,
    std::stop_token stopToken = {},
    std::function<void(NativeModelDownloadProgress)> progress = {});

} // namespace rifelove::native_desktop

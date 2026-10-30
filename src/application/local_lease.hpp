#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace rifelove::application {
struct FileIdentity {
    std::uint64_t size = 0;
    std::uint64_t modified = 0;
    std::string fileId;
    std::string sha256;
};
// A native read lease prevents writes, deletion and path replacement while
// FFmpeg independently reads the source. No payload is retained in memory.
class SourceLease final {
public:
    explicit SourceLease(const std::filesystem::path& path);
    ~SourceLease();
    SourceLease(const SourceLease&) = delete;
    SourceLease& operator=(const SourceLease&) = delete;
    [[nodiscard]] FileIdentity fingerprint(std::stop_token stop, const std::function<void(std::uint64_t,std::uint64_t)>& progress = {});
private:
    void* handle_ = nullptr;
};
// Ownership is a live OS handle, not the presence of a stale marker file.
class QueueOwner final {
public:
    explicit QueueOwner(const std::filesystem::path& database);
    ~QueueOwner();
    QueueOwner(const QueueOwner&) = delete;
    QueueOwner& operator=(const QueueOwner&) = delete;
private:
    void* handle_ = nullptr;
};
} // namespace rifelove::application

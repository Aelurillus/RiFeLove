#pragma once

#include "application/local_lease.hpp"
#include "database/task_store.hpp"
#include "media/media_engine.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace rifelove::application {

// Owns the task-scoped local segment directory and binds finalized media files
// to the SQLite journal only after hashing and full media-contract validation.
class CheckpointArtifacts final {
public:
    CheckpointArtifacts(TaskStore& store, std::string taskId, std::filesystem::path outputPath);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::filesystem::path segmentPath(std::uint64_t index) const;
    void setManifest(const std::string& manifest);
    [[nodiscard]] std::string manifest() const;
    [[nodiscard]] std::vector<media::VideoSegmentInput> verifyCommitted(std::stop_token stop = {}) const;
    // Remove only unjournaled segment outputs inside this task-owned directory.
    // Call after the manifest and committed segments have been verified.
    [[nodiscard]] std::size_t removeUncommittedSegments(std::stop_token stop = {}) const;
    [[nodiscard]] TaskCheckpointSegment commit(std::uint64_t index, std::uint64_t firstOutputFrame,
        std::uint64_t endOutputFrame, const media::VideoSegmentInput& segment, std::stop_token stop = {});

    [[nodiscard]] static std::string serializeContract(const media::VideoSegmentContract& contract);
    [[nodiscard]] static media::VideoSegmentContract parseContract(const std::string& json);
    // Schema 1 checkpoints predate adapter identity. They may be upgraded in
    // memory only as the built-in RIFE SDR adapter; custom identities fail closed.
    [[nodiscard]] static bool manifestMatches(const std::string& saved, const std::string& expected);

private:
    void ensureDirectory() const;
    [[nodiscard]] std::filesystem::path resolveRecordedPath(const TaskCheckpointSegment& segment) const;

    TaskStore& store_;
    std::string taskId_;
    std::filesystem::path directory_;
};

} // namespace rifelove::application

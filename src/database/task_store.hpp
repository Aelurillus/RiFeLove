#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <stop_token>
#include <cstdint>

struct sqlite3;

namespace rifelove {

enum class TaskState { queued, running, paused, interrupted, failed, cancelled, completed };
[[nodiscard]] std::string toString(TaskState state);
[[nodiscard]] TaskState taskStateFromString(const std::string& state);

struct TaskRecord {
    std::string id;
    std::string sourcePath;
    std::string outputPath;
    TaskState state;
    std::string createdAt;
    std::string updatedAt;
    std::string error;
};
struct ConfiguredTask {
    TaskRecord task;
    std::string configuration;
    std::string identity;
    std::string progress;
    std::string result;
};
struct NewConfiguredTask { std::string sourcePath; std::string outputPath; std::string configuration; };
struct TaskCheckpointSegment {
    std::uint64_t segmentIndex = 0;
    std::uint64_t firstOutputFrame = 0;
    std::uint64_t endOutputFrame = 0;
    std::string artifactPath;
    std::string sha256;
    std::uint64_t byteCount = 0;
    // Versioned JSON containing exact PTS boundaries and the validated stream contract.
    std::string contract;
};

class TaskStore final {
public:
    enum class OpenMode { create, readWrite, readOnly };
    explicit TaskStore(const std::string& databasePath, OpenMode mode = OpenMode::create);
    ~TaskStore();
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    [[nodiscard]] std::string create(const std::string& sourcePath, const std::string& outputPath);
    void transition(const std::string& id, TaskState next, const std::string& error = {});
    // Narrow transition for the queue after it has verified immutable identity
    // and all local checkpoint artifacts.
    void resumeCheckpointed(const std::string& id);
    [[nodiscard]] std::size_t recoverInterrupted();
    [[nodiscard]] std::vector<TaskRecord> list() const;
    // Read-only, single-query snapshot used by reports and diagnostics. Unlike
    // configured(), this also includes legacy tasks without task_details.
    [[nodiscard]] std::vector<ConfiguredTask> reportSnapshot() const;
    // Payloads are versioned JSON owned/validated by application services.
    // Batch creation and terminal state/result publication are transactional.
    [[nodiscard]] std::vector<std::string> createConfigured(const std::vector<NewConfiguredTask>& tasks, std::stop_token stop = {});
    [[nodiscard]] std::vector<ConfiguredTask> configured() const;
    void saveIdentity(const std::string& id, const std::string& json);
    void saveProgress(const std::string& id, const std::string& json);
    void finish(const std::string& id, TaskState state, const std::string& result, const std::string& error = {});
    [[nodiscard]] std::size_t recoverConfigured();

    // A checkpoint manifest is immutable for a task. Segment records are added
    // only after the caller has closed, validated and hashed the local artifact.
    void setCheckpointManifest(const std::string& id, const std::string& manifest);
    [[nodiscard]] std::string checkpointManifest(const std::string& id) const;
    void recordCheckpointSegment(const std::string& id, const TaskCheckpointSegment& segment);
    [[nodiscard]] std::vector<TaskCheckpointSegment> checkpointSegments(const std::string& id) const;

private:
    sqlite3* db_{};
    OpenMode mode_;
    mutable std::mutex mutex_;
    void requireWritable() const;
};

} // namespace rifelove

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <optional>

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

class TaskStore final {
public:
    enum class OpenMode { create, readWrite, readOnly };
    explicit TaskStore(const std::string& databasePath, OpenMode mode = OpenMode::create);
    ~TaskStore();
    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    [[nodiscard]] std::string create(const std::string& sourcePath, const std::string& outputPath);
    void transition(const std::string& id, TaskState next, const std::string& error = {});
    [[nodiscard]] std::size_t recoverInterrupted();
    [[nodiscard]] std::vector<TaskRecord> list() const;
    // Payloads are versioned JSON owned/validated by application services.
    // Batch creation and terminal state/result publication are transactional.
    [[nodiscard]] std::vector<std::string> createConfigured(const std::vector<NewConfiguredTask>& tasks);
    [[nodiscard]] std::vector<ConfiguredTask> configured() const;
    void saveIdentity(const std::string& id, const std::string& json);
    void saveProgress(const std::string& id, const std::string& json);
    void finish(const std::string& id, TaskState state, const std::string& result, const std::string& error = {});
    [[nodiscard]] std::size_t recoverConfigured();

private:
    sqlite3* db_{};
    OpenMode mode_;
    mutable std::mutex mutex_;
    void requireWritable() const;
};

} // namespace rifelove

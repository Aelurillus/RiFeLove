#pragma once

#include <string>
#include <vector>

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

private:
    sqlite3* db_{};
    OpenMode mode_;
    void requireWritable() const;
};

} // namespace rifelove

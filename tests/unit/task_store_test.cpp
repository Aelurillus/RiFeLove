#include "database/task_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>

namespace {
struct TemporaryDatabase {
    std::filesystem::path directory;
    std::filesystem::path path;
    TemporaryDatabase() {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            directory = std::filesystem::temp_directory_path() / ("rifelove-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory)) {
                path = directory / "queue.sqlite";
                return;
            }
        }
        throw std::runtime_error("could not reserve temporary test directory");
    }
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
        std::filesystem::remove(directory, error);
    }
};
}

TEST(TaskStore, ReadOnlyInspectionDoesNotChangeActiveTasks) {
    TemporaryDatabase database;
    std::string id;
    {
        rifelove::TaskStore writable(database.path.string());
        id = writable.create("input.mp4", "output.mp4");
        writable.transition(id, rifelove::TaskState::running);
    }
    rifelove::TaskStore reader(database.path.string(), rifelove::TaskStore::OpenMode::readOnly);
    const auto tasks = reader.list();
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().state, rifelove::TaskState::running);
    EXPECT_THROW((void)reader.recoverInterrupted(), std::logic_error);
    EXPECT_THROW(reader.transition(id, rifelove::TaskState::cancelled), std::logic_error);
    EXPECT_THROW((void)reader.create("another.mp4", "out.mp4"), std::logic_error);
}

TEST(TaskStore, InspectionAndMaintenanceDoNotCreateMissingDatabases) {
    TemporaryDatabase database;
    EXPECT_THROW((rifelove::TaskStore(database.path.string(), rifelove::TaskStore::OpenMode::readOnly)), std::runtime_error);
    EXPECT_THROW((rifelove::TaskStore(database.path.string(), rifelove::TaskStore::OpenMode::readWrite)), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(database.path));
}

TEST(TaskStore, EnforcesStateMachineAndPersistsHistory) {
    TemporaryDatabase database;
    std::string id;
    {
        rifelove::TaskStore store(database.path.string());
        id = store.create("input.mp4", "output.mp4");
        EXPECT_THROW(store.transition(id, rifelove::TaskState::completed), std::logic_error);
        store.transition(id, rifelove::TaskState::running);
        store.transition(id, rifelove::TaskState::paused);
        store.transition(id, rifelove::TaskState::running);
        store.transition(id, rifelove::TaskState::completed);
    }
    rifelove::TaskStore reopened(database.path.string());
    const auto tasks = reopened.list();
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().id, id);
    EXPECT_EQ(tasks.front().state, rifelove::TaskState::completed);
}

TEST(TaskStore, MakesRunningTasksVisibleAfterCrashWithoutClaimingResume) {
    TemporaryDatabase database;
    {
        rifelove::TaskStore store(database.path.string());
        const auto id = store.create("input.mkv", "output.mkv");
        store.transition(id, rifelove::TaskState::running);
    }
    rifelove::TaskStore restarted(database.path.string());
    EXPECT_EQ(restarted.recoverInterrupted(), 1U);
    const auto tasks = restarted.list();
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks[0].state, rifelove::TaskState::interrupted);
    EXPECT_NE(tasks[0].error.find("resume is not yet implemented"), std::string::npos);
    EXPECT_THROW(restarted.transition(tasks[0].id, rifelove::TaskState::running), std::logic_error);
    EXPECT_EQ(restarted.recoverInterrupted(), 0U);
}

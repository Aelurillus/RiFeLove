#include "database/task_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {
struct TemporaryDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("rifelove-test-" + std::to_string(std::rand()) + ".sqlite");
    ~TemporaryDatabase() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
    }
};
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

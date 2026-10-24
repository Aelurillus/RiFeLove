#include "database/task_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <sqlite3.h>

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

TEST(TaskStore, ConfiguredBatchPersistsOrderConfigurationAndTerminalEvidence) {
    TemporaryDatabase database;
    std::vector<std::string> ids;
    {
        rifelove::TaskStore store(database.path.string());
        ids = store.createConfigured({{"a.mkv","a-60.mkv","{\"fps\":\"60\"}"},{"b.mkv","b-144.mkv","{\"fps\":\"144\"}"}});
        store.transition(ids[0],rifelove::TaskState::running);
        store.saveIdentity(ids[0],"{\"source_sha256\":\"example\"}");
        store.saveProgress(ids[0],"{\"frames\":15}");
        store.transition(ids[0],rifelove::TaskState::paused);
        store.finish(ids[0],rifelove::TaskState::cancelled,{},"user cancellation");
        EXPECT_THROW(store.finish(ids[0],rifelove::TaskState::completed,"{}"),std::logic_error);
        EXPECT_THROW(store.saveProgress(ids[0],"{}"),std::logic_error);
    }
    rifelove::TaskStore store(database.path.string());
    const auto rows = store.configured();
    ASSERT_EQ(rows.size(),2U); EXPECT_EQ(rows[0].task.id,ids[0]); EXPECT_EQ(rows[1].task.id,ids[1]);
    EXPECT_EQ(rows[0].identity,"{\"source_sha256\":\"example\"}"); EXPECT_EQ(rows[0].progress,"{\"frames\":15}");
    EXPECT_EQ(rows[0].task.error,"user cancellation"); EXPECT_EQ(rows[1].configuration,"{\"fps\":\"144\"}");
    EXPECT_THROW((void)store.createConfigured({{"c","d","{}"},{"e","f",std::string(65537,'x')}}),std::invalid_argument);
    EXPECT_EQ(store.configured().size(),2U);
}

TEST(TaskStore, ConfiguredRecoveryIncludesPausedAndDoesNotModifyLegacyWorkers) {
    TemporaryDatabase database;
    rifelove::TaskStore store(database.path.string());
    const auto legacy = store.create("legacy.mkv","legacy-out.mkv"); store.transition(legacy,rifelove::TaskState::running);
    const auto id = store.createConfigured({{"a.mkv","out.mkv","{}"}}).front();
    store.transition(id,rifelove::TaskState::running); store.transition(id,rifelove::TaskState::paused);
    EXPECT_EQ(store.recoverConfigured(),1U); EXPECT_EQ(store.recoverConfigured(),0U);
    const auto rows = store.list();
    for (const auto& row : rows) EXPECT_EQ(row.state,row.id == legacy ? rifelove::TaskState::running : rifelove::TaskState::interrupted);
    EXPECT_THROW(store.transition(id,rifelove::TaskState::queued),std::logic_error);
}

TEST(TaskStore, RollsBackWholeBatchWhenALaterDatabaseWriteFails) {
    TemporaryDatabase database;
    rifelove::TaskStore store(database.path.string());
    sqlite3* connection = nullptr;
    ASSERT_EQ(sqlite3_open(database.path.string().c_str(),&connection),SQLITE_OK);
    const int status = sqlite3_exec(connection,"CREATE TRIGGER reject_task BEFORE INSERT ON task_details WHEN NEW.configuration='reject' BEGIN SELECT RAISE(ABORT,'injected task failure'); END",nullptr,nullptr,nullptr);
    sqlite3_close(connection); ASSERT_EQ(status,SQLITE_OK);
    EXPECT_THROW((void)store.createConfigured({{"a","out-a","{}"},{"b","out-b","reject"}}),std::runtime_error);
    EXPECT_TRUE(store.configured().empty()); EXPECT_TRUE(store.list().empty());
}

TEST(TaskStore, RetryClearsAttemptProgressAndResultButRetainsIdentity) {
    TemporaryDatabase database;
    rifelove::TaskStore store(database.path.string());
    const auto id = store.createConfigured({{"a","out-a","{}"}}).front();
    store.transition(id,rifelove::TaskState::running); store.saveIdentity(id,"{\"sha256\":\"bound\"}");
    store.saveProgress(id,"{\"frames\":25}"); store.finish(id,rifelove::TaskState::failed,"{\"diagnostic\":true}","encoder failed");
    store.transition(id,rifelove::TaskState::queued);
    const auto row = store.configured().front();
    EXPECT_EQ(row.identity,"{\"sha256\":\"bound\"}"); EXPECT_TRUE(row.progress.empty()); EXPECT_TRUE(row.result.empty()); EXPECT_TRUE(row.task.error.empty());
}

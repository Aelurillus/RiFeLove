#include "database/task_store.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace rifelove {
namespace {

void execute(sqlite3* db, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string error = message ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}

class Statement final {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3_stmt* statement_{};
};

void bindText(sqlite3* db, sqlite3_stmt* statement, int index, const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
std::string columnText(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

bool allowed(TaskState from, TaskState to) {
    switch (from) {
    case TaskState::queued: return to == TaskState::running || to == TaskState::cancelled;
    case TaskState::running: return to == TaskState::paused || to == TaskState::interrupted || to == TaskState::failed || to == TaskState::cancelled || to == TaskState::completed;
    case TaskState::paused: return to == TaskState::running || to == TaskState::cancelled;
    case TaskState::interrupted: return to == TaskState::cancelled;
    case TaskState::failed: return to == TaskState::queued;
    case TaskState::cancelled: case TaskState::completed: return false;
    }
    return false;
}

} // namespace

std::string toString(TaskState state) {
    switch (state) {
    case TaskState::queued: return "queued"; case TaskState::running: return "running"; case TaskState::paused: return "paused";
    case TaskState::interrupted: return "interrupted"; case TaskState::failed: return "failed";
    case TaskState::cancelled: return "cancelled"; case TaskState::completed: return "completed";
    }
    throw std::invalid_argument("unknown task state");
}

TaskState taskStateFromString(const std::string& state) {
    if (state == "queued") return TaskState::queued;
    if (state == "running") return TaskState::running;
    if (state == "paused") return TaskState::paused;
    if (state == "interrupted") return TaskState::interrupted;
    if (state == "failed") return TaskState::failed;
    if (state == "cancelled") return TaskState::cancelled;
    if (state == "completed") return TaskState::completed;
    throw std::invalid_argument("unknown persisted task state: " + state);
}

TaskStore::TaskStore(const std::string& databasePath) {
    if (sqlite3_open_v2(databasePath.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "could not allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(db_, 5000);
    try {
        execute(db_, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
        execute(db_, "CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, source_path TEXT NOT NULL, output_path TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('queued','running','paused','interrupted','failed','cancelled','completed')), created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, error TEXT NOT NULL DEFAULT ''); CREATE INDEX IF NOT EXISTS tasks_state_created ON tasks(state, created_at);");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
TaskStore::~TaskStore() { if (db_) sqlite3_close(db_); }

std::string TaskStore::create(const std::string& sourcePath, const std::string& outputPath) {
    if (sourcePath.empty() || outputPath.empty()) throw std::invalid_argument("source and output paths must not be empty");
    Statement statement(db_, "INSERT INTO tasks(id,source_path,output_path,state) VALUES(lower(hex(randomblob(16))),?,?, 'queued') RETURNING id");
    bindText(db_, statement.get(), 1, sourcePath); bindText(db_, statement.get(), 2, outputPath);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
    return columnText(statement.get(), 0);
}

void TaskStore::transition(const std::string& id, TaskState next, const std::string& error) {
    Statement read(db_, "SELECT state FROM tasks WHERE id=?"); bindText(db_, read.get(), 1, id);
    if (sqlite3_step(read.get()) != SQLITE_ROW) throw std::invalid_argument("task does not exist: " + id);
    const auto current = taskStateFromString(columnText(read.get(), 0));
    if (!allowed(current, next)) throw std::logic_error("invalid task transition: " + toString(current) + " -> " + toString(next));
    Statement update(db_, "UPDATE tasks SET state=?, error=?, updated_at=CURRENT_TIMESTAMP WHERE id=? AND state=?");
    bindText(db_, update.get(), 1, toString(next)); bindText(db_, update.get(), 2, error); bindText(db_, update.get(), 3, id); bindText(db_, update.get(), 4, toString(current));
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1) throw std::runtime_error("task changed concurrently; reload before retrying");
}

std::size_t TaskStore::recoverInterrupted() {
    execute(db_, "UPDATE tasks SET state='interrupted',error='Process ended while task was running; checkpoint resume is not yet implemented.',updated_at=CURRENT_TIMESTAMP WHERE state='running'");
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

std::vector<TaskRecord> TaskStore::list() const {
    Statement statement(db_, "SELECT id,source_path,output_path,state,created_at,updated_at,error FROM tasks ORDER BY created_at,id");
    std::vector<TaskRecord> result;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
        result.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2), taskStateFromString(columnText(statement.get(), 3)), columnText(statement.get(), 4), columnText(statement.get(), 5), columnText(statement.get(), 6)});
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return result;
}

} // namespace rifelove

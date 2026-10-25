#include "database/task_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <limits>
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
void bindInt64(sqlite3* db, sqlite3_stmt* statement, int index, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
        throw std::invalid_argument("checkpoint integer exceeds SQLite's signed 64-bit range");
    if (sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
std::string columnText(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}
class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

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

TaskStore::TaskStore(const std::string& databasePath, OpenMode mode) : mode_(mode) {
    const int access = mode == OpenMode::readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    const int create = mode == OpenMode::create ? SQLITE_OPEN_CREATE : 0;
    if (sqlite3_open_v2(databasePath.c_str(), &db_, access | create | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "could not allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(db_, 5000);
    if (mode_ == OpenMode::readOnly) return;
    try {
        execute(db_, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
        execute(db_, "CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, source_path TEXT NOT NULL, output_path TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('queued','running','paused','interrupted','failed','cancelled','completed')), created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, error TEXT NOT NULL DEFAULT ''); CREATE INDEX IF NOT EXISTS tasks_state_created ON tasks(state, created_at);");
        execute(db_, "CREATE TABLE IF NOT EXISTS task_details (sequence INTEGER PRIMARY KEY AUTOINCREMENT, task_id TEXT NOT NULL UNIQUE REFERENCES tasks(id), configuration TEXT NOT NULL, identity TEXT NOT NULL DEFAULT '', progress TEXT NOT NULL DEFAULT '', result TEXT NOT NULL DEFAULT '');");
        execute(db_, "CREATE TABLE IF NOT EXISTS task_checkpoint_manifests (task_id TEXT PRIMARY KEY REFERENCES task_details(task_id) ON DELETE CASCADE, schema_version INTEGER NOT NULL CHECK(schema_version=1), manifest_json TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
        execute(db_, "CREATE TABLE IF NOT EXISTS task_checkpoint_segments (task_id TEXT NOT NULL REFERENCES task_details(task_id) ON DELETE CASCADE, segment_index INTEGER NOT NULL CHECK(segment_index>=0), first_output_frame INTEGER NOT NULL CHECK(first_output_frame>=0), end_output_frame INTEGER NOT NULL CHECK(end_output_frame>first_output_frame), artifact_path TEXT NOT NULL, sha256 TEXT NOT NULL CHECK(length(sha256)=64), byte_count INTEGER NOT NULL CHECK(byte_count>0), contract_json TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(task_id,segment_index), UNIQUE(task_id,artifact_path));");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
TaskStore::~TaskStore() { if (db_) sqlite3_close(db_); }

void TaskStore::requireWritable() const {
    if (mode_ == OpenMode::readOnly) throw std::logic_error("task store is read-only");
}

std::string TaskStore::create(const std::string& sourcePath, const std::string& outputPath) {
    std::lock_guard lock(mutex_);
    requireWritable();
    if (sourcePath.empty() || outputPath.empty()) throw std::invalid_argument("source and output paths must not be empty");
    Statement statement(db_, "INSERT INTO tasks(id,source_path,output_path,state) VALUES(lower(hex(randomblob(16))),?,?, 'queued') RETURNING id");
    bindText(db_, statement.get(), 1, sourcePath); bindText(db_, statement.get(), 2, outputPath);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
    return columnText(statement.get(), 0);
}

void TaskStore::transition(const std::string& id, TaskState next, const std::string& error) {
    std::lock_guard lock(mutex_);
    requireWritable();
    Transaction transaction(db_);
    Statement read(db_, "SELECT state FROM tasks WHERE id=?"); bindText(db_, read.get(), 1, id);
    if (sqlite3_step(read.get()) != SQLITE_ROW) throw std::invalid_argument("task does not exist: " + id);
    const auto current = taskStateFromString(columnText(read.get(), 0));
    if (!allowed(current, next)) throw std::logic_error("invalid task transition: " + toString(current) + " -> " + toString(next));
    Statement update(db_, "UPDATE tasks SET state=?, error=?, updated_at=CURRENT_TIMESTAMP WHERE id=? AND state=?");
    bindText(db_, update.get(), 1, toString(next)); bindText(db_, update.get(), 2, error); bindText(db_, update.get(), 3, id); bindText(db_, update.get(), 4, toString(current));
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1) throw std::runtime_error("task changed concurrently; reload before retrying");
    if (next == TaskState::queued) {
        Statement reset(db_, "UPDATE task_details SET progress='',result='' WHERE task_id=?");
        bindText(db_,reset.get(),1,id);
        if (sqlite3_step(reset.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    }
    transaction.commit();
}

void TaskStore::resumeCheckpointed(const std::string& id) {
    std::lock_guard lock(mutex_); requireWritable();
    Transaction transaction(db_);
    Statement state(db_,"SELECT state FROM tasks WHERE id=? AND EXISTS(SELECT 1 FROM task_details WHERE task_id=id)");
    bindText(db_,state.get(),1,id);
    if (sqlite3_step(state.get()) != SQLITE_ROW) throw std::invalid_argument("configured task does not exist");
    if (taskStateFromString(columnText(state.get(),0)) != TaskState::interrupted)
        throw std::logic_error("only interrupted tasks can be resumed from a checkpoint");
    Statement checkpoint(db_,"SELECT 1 FROM task_checkpoint_manifests m WHERE m.task_id=? AND EXISTS(SELECT 1 FROM task_checkpoint_segments s WHERE s.task_id=m.task_id)");
    bindText(db_,checkpoint.get(),1,id);
    if (sqlite3_step(checkpoint.get()) != SQLITE_ROW)
        throw std::logic_error("checkpoint resume requires an immutable manifest and at least one committed segment");
    Statement update(db_,"UPDATE tasks SET state='queued',error='',updated_at=CURRENT_TIMESTAMP WHERE id=? AND state='interrupted'");
    bindText(db_,update.get(),1,id);
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        throw std::runtime_error("checkpoint resume state transition failed");
    transaction.commit();
}

std::size_t TaskStore::recoverInterrupted() {
    std::lock_guard lock(mutex_);
    requireWritable();
    execute(db_, "UPDATE tasks SET state='interrupted',error='Process ended while task was running or paused; checkpoint resume is not yet implemented.',updated_at=CURRENT_TIMESTAMP WHERE state IN ('running','paused')");
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

std::vector<TaskRecord> TaskStore::list() const {
    std::lock_guard lock(mutex_);
    Statement statement(db_, "SELECT id,source_path,output_path,state,created_at,updated_at,error FROM tasks ORDER BY created_at,id");
    std::vector<TaskRecord> result;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
        result.push_back({columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2), taskStateFromString(columnText(statement.get(), 3)), columnText(statement.get(), 4), columnText(statement.get(), 5), columnText(statement.get(), 6)});
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return result;
}

namespace {
void boundedPayload(const std::string& value, bool required = false) {
    if ((required && value.empty()) || value.size() > 64 * 1024 || value.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid or oversized task payload");
}
bool validSha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}
std::vector<std::string> TaskStore::createConfigured(const std::vector<NewConfiguredTask>& tasks, std::stop_token stop) {
    std::lock_guard lock(mutex_); requireWritable();
    if (tasks.empty() || tasks.size() > 10000) throw std::invalid_argument("Queue batch must contain 1 to 10000 tasks");
    for (const auto& task : tasks) {
        if (stop.stop_requested()) throw std::runtime_error("Task import cancelled");
        if (task.sourcePath.empty() || task.outputPath.empty()) throw std::invalid_argument("Task paths are required");
        boundedPayload(task.configuration, true);
    }
    Transaction transaction(db_);
    std::vector<std::string> ids;
    for (const auto& task : tasks) {
        if (stop.stop_requested()) throw std::runtime_error("Task import cancelled");
        std::string id;
        {
            Statement insert(db_, "INSERT INTO tasks(id,source_path,output_path,state) VALUES(lower(hex(randomblob(16))),?,?,'queued') RETURNING id");
            bindText(db_,insert.get(),1,task.sourcePath); bindText(db_,insert.get(),2,task.outputPath);
            if (sqlite3_step(insert.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_));
            id = columnText(insert.get(),0);
        }
        Statement detail(db_, "INSERT INTO task_details(task_id,configuration) VALUES(?,?)");
        bindText(db_,detail.get(),1,id); bindText(db_,detail.get(),2,task.configuration);
        if (sqlite3_step(detail.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        ids.push_back(std::move(id));
    }
    if (stop.stop_requested()) throw std::runtime_error("Task import cancelled");
    transaction.commit(); return ids;
}
std::vector<ConfiguredTask> TaskStore::configured() const {
    std::lock_guard lock(mutex_);
    Statement statement(db_, "SELECT t.id,t.source_path,t.output_path,t.state,t.created_at,t.updated_at,t.error,d.configuration,d.identity,d.progress,d.result FROM tasks t JOIN task_details d ON t.id=d.task_id ORDER BY d.sequence");
    std::vector<ConfiguredTask> result;
    int status;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
        result.push_back({{columnText(statement.get(),0),columnText(statement.get(),1),columnText(statement.get(),2),taskStateFromString(columnText(statement.get(),3)),columnText(statement.get(),4),columnText(statement.get(),5),columnText(statement.get(),6)},columnText(statement.get(),7),columnText(statement.get(),8),columnText(statement.get(),9),columnText(statement.get(),10)});
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return result;
}
std::vector<ConfiguredTask> TaskStore::reportSnapshot() const {
    std::lock_guard lock(mutex_);
    Statement statement(db_, "SELECT t.id,t.source_path,t.output_path,t.state,t.created_at,t.updated_at,t.error,COALESCE(d.configuration,''),COALESCE(d.identity,''),COALESCE(d.progress,''),COALESCE(d.result,'') FROM tasks t LEFT JOIN task_details d ON t.id=d.task_id ORDER BY t.created_at,t.id");
    std::vector<ConfiguredTask> result;
    int status;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
        result.push_back({{columnText(statement.get(),0),columnText(statement.get(),1),columnText(statement.get(),2),taskStateFromString(columnText(statement.get(),3)),columnText(statement.get(),4),columnText(statement.get(),5),columnText(statement.get(),6)},columnText(statement.get(),7),columnText(statement.get(),8),columnText(statement.get(),9),columnText(statement.get(),10)});
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return result;
}
void TaskStore::saveIdentity(const std::string& id, const std::string& json) {
    std::lock_guard lock(mutex_); requireWritable(); boundedPayload(json,true);
    Statement update(db_, "UPDATE task_details SET identity=? WHERE task_id=? AND EXISTS(SELECT 1 FROM tasks WHERE id=task_id AND state='running')");
    bindText(db_,update.get(),1,json); bindText(db_,update.get(),2,id);
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1) throw std::logic_error("Identity requires a running configured task");
}
void TaskStore::saveProgress(const std::string& id, const std::string& json) {
    std::lock_guard lock(mutex_); requireWritable(); boundedPayload(json,true);
    Statement update(db_, "UPDATE task_details SET progress=? WHERE task_id=? AND EXISTS(SELECT 1 FROM tasks WHERE id=task_id AND state IN ('running','paused'))");
    bindText(db_,update.get(),1,json); bindText(db_,update.get(),2,id);
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1) throw std::logic_error("Progress requires an active configured task");
}
void TaskStore::finish(const std::string& id, TaskState state, const std::string& result, const std::string& error) {
    std::lock_guard lock(mutex_); requireWritable(); boundedPayload(result);
    if (state != TaskState::completed && state != TaskState::failed && state != TaskState::cancelled) throw std::invalid_argument("Invalid terminal task state");
    Transaction transaction(db_);
    Statement update(db_, "UPDATE tasks SET state=?,error=?,updated_at=CURRENT_TIMESTAMP WHERE id=? AND state IN ('running','paused') AND EXISTS(SELECT 1 FROM task_details WHERE task_id=id)");
    bindText(db_,update.get(),1,toString(state)); bindText(db_,update.get(),2,error); bindText(db_,update.get(),3,id);
    if (sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(db_) != 1) throw std::logic_error("Task is not active/configured");
    Statement detail(db_, "UPDATE task_details SET result=? WHERE task_id=?");
    bindText(db_,detail.get(),1,result); bindText(db_,detail.get(),2,id);
    if (sqlite3_step(detail.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    transaction.commit();
}
std::size_t TaskStore::recoverConfigured() {
    std::lock_guard lock(mutex_); requireWritable();
    execute(db_, "UPDATE tasks SET state='interrupted',error='Worker ended while task was active. Verify checkpoints to resume or create a new output to restart.',updated_at=CURRENT_TIMESTAMP WHERE state IN ('running','paused') AND EXISTS(SELECT 1 FROM task_details WHERE task_id=id)");
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

void TaskStore::setCheckpointManifest(const std::string& id, const std::string& manifest) {
    boundedPayload(manifest,true);
    std::lock_guard lock(mutex_);
    requireWritable();
    Transaction transaction(db_);
    Statement state(db_,"SELECT state FROM tasks WHERE id=? AND EXISTS(SELECT 1 FROM task_details WHERE task_id=id)");
    bindText(db_,state.get(),1,id);
    if (sqlite3_step(state.get()) != SQLITE_ROW) throw std::invalid_argument("configured task does not exist");
    const auto current = taskStateFromString(columnText(state.get(),0));
    if (current != TaskState::running && current != TaskState::paused)
        throw std::logic_error("checkpoint manifest requires an active configured task");

    Statement existing(db_,"SELECT manifest_json FROM task_checkpoint_manifests WHERE task_id=?");
    bindText(db_,existing.get(),1,id);
    const int status = sqlite3_step(existing.get());
    if (status == SQLITE_ROW) {
        if (columnText(existing.get(),0) != manifest)
            throw std::logic_error("checkpoint manifest is immutable; task identity or processing settings changed");
        transaction.commit();
        return;
    }
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    Statement insert(db_,"INSERT INTO task_checkpoint_manifests(task_id,schema_version,manifest_json) VALUES(?,1,?)");
    bindText(db_,insert.get(),1,id); bindText(db_,insert.get(),2,manifest);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    transaction.commit();
}

std::string TaskStore::checkpointManifest(const std::string& id) const {
    std::lock_guard lock(mutex_);
    Statement statement(db_,"SELECT manifest_json FROM task_checkpoint_manifests WHERE task_id=?");
    bindText(db_,statement.get(),1,id);
    const int status = sqlite3_step(statement.get());
    if (status == SQLITE_ROW) return columnText(statement.get(),0);
    if (status == SQLITE_DONE) return {};
    throw std::runtime_error(sqlite3_errmsg(db_));
}

void TaskStore::recordCheckpointSegment(const std::string& id, const TaskCheckpointSegment& segment) {
    if (segment.firstOutputFrame >= segment.endOutputFrame || segment.artifactPath.empty() ||
        segment.artifactPath.size() > 32767 || segment.artifactPath.find('\0') != std::string::npos ||
        !validSha256(segment.sha256) || segment.byteCount == 0)
        throw std::invalid_argument("checkpoint segment metadata is invalid");
    boundedPayload(segment.contract,true);
    std::lock_guard lock(mutex_);
    requireWritable();
    Transaction transaction(db_);
    Statement state(db_,"SELECT state FROM tasks WHERE id=? AND EXISTS(SELECT 1 FROM task_details WHERE task_id=id)");
    bindText(db_,state.get(),1,id);
    if (sqlite3_step(state.get()) != SQLITE_ROW) throw std::invalid_argument("configured task does not exist");
    const auto current = taskStateFromString(columnText(state.get(),0));
    if (current != TaskState::running && current != TaskState::paused)
        throw std::logic_error("checkpoint segments require an active configured task");
    Statement manifest(db_,"SELECT 1 FROM task_checkpoint_manifests WHERE task_id=?");
    bindText(db_,manifest.get(),1,id);
    if (sqlite3_step(manifest.get()) != SQLITE_ROW) throw std::logic_error("checkpoint manifest must be committed before any segment");

    std::uint64_t expectedIndex = 0;
    std::uint64_t expectedFrame = 0;
    Statement previous(db_,"SELECT segment_index,end_output_frame FROM task_checkpoint_segments WHERE task_id=? ORDER BY segment_index DESC LIMIT 1");
    bindText(db_,previous.get(),1,id);
    const int previousStatus = sqlite3_step(previous.get());
    if (previousStatus == SQLITE_ROW) {
        const auto lastIndex = sqlite3_column_int64(previous.get(),0);
        const auto lastFrame = sqlite3_column_int64(previous.get(),1);
        if (lastIndex < 0 || lastFrame < 0 || lastIndex == std::numeric_limits<sqlite3_int64>::max())
            throw std::runtime_error("stored checkpoint segment chain is invalid");
        expectedIndex = static_cast<std::uint64_t>(lastIndex + 1);
        expectedFrame = static_cast<std::uint64_t>(lastFrame);
    } else if (previousStatus != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    if (segment.segmentIndex != expectedIndex || segment.firstOutputFrame != expectedFrame)
        throw std::logic_error("checkpoint segments must form a contiguous output-frame prefix");

    Statement insert(db_,"INSERT INTO task_checkpoint_segments(task_id,segment_index,first_output_frame,end_output_frame,artifact_path,sha256,byte_count,contract_json) VALUES(?,?,?,?,?,?,?,?)");
    bindText(db_,insert.get(),1,id); bindInt64(db_,insert.get(),2,segment.segmentIndex);
    bindInt64(db_,insert.get(),3,segment.firstOutputFrame); bindInt64(db_,insert.get(),4,segment.endOutputFrame);
    bindText(db_,insert.get(),5,segment.artifactPath); bindText(db_,insert.get(),6,segment.sha256);
    bindInt64(db_,insert.get(),7,segment.byteCount); bindText(db_,insert.get(),8,segment.contract);
    if (sqlite3_step(insert.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    transaction.commit();
}

std::vector<TaskCheckpointSegment> TaskStore::checkpointSegments(const std::string& id) const {
    std::lock_guard lock(mutex_);
    Statement statement(db_,"SELECT segment_index,first_output_frame,end_output_frame,artifact_path,sha256,byte_count,contract_json FROM task_checkpoint_segments WHERE task_id=? ORDER BY segment_index");
    bindText(db_,statement.get(),1,id);
    std::vector<TaskCheckpointSegment> result;
    std::uint64_t expectedIndex = 0;
    std::uint64_t expectedFrame = 0;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto segmentIndex = sqlite3_column_int64(statement.get(),0);
        const auto firstFrame = sqlite3_column_int64(statement.get(),1);
        const auto endFrame = sqlite3_column_int64(statement.get(),2);
        const auto byteCount = sqlite3_column_int64(statement.get(),5);
        const auto artifactPath = columnText(statement.get(),3);
        const auto sha256 = columnText(statement.get(),4);
        if (segmentIndex < 0 || firstFrame < 0 || endFrame <= firstFrame || byteCount <= 0 ||
            static_cast<std::uint64_t>(segmentIndex) != expectedIndex || static_cast<std::uint64_t>(firstFrame) != expectedFrame ||
            artifactPath.empty() || !validSha256(sha256))
            throw std::runtime_error("stored checkpoint segment metadata is invalid");
        if (expectedIndex == static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
            endFrame == std::numeric_limits<sqlite3_int64>::max())
            throw std::runtime_error("stored checkpoint segment chain exceeds supported frame limits");
        ++expectedIndex;
        expectedFrame = static_cast<std::uint64_t>(endFrame);
        result.push_back({static_cast<std::uint64_t>(segmentIndex),static_cast<std::uint64_t>(firstFrame),
            static_cast<std::uint64_t>(endFrame),artifactPath,sha256,
            static_cast<std::uint64_t>(byteCount),columnText(statement.get(),6)});
    }
    if (status != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
    return result;
}

} // namespace rifelove

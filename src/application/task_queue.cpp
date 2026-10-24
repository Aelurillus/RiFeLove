#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::application {
namespace {
std::string utf8(const std::filesystem::path& path) { const auto value = path.u8string(); return {value.begin(),value.end()}; }
std::filesystem::path path(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
bool samePath(const std::string& a, const std::string& b) {
    const auto left = std::filesystem::weakly_canonical(std::filesystem::absolute(path(a))).wstring();
    const auto right = std::filesystem::weakly_canonical(std::filesystem::absolute(path(b))).wstring();
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE) == CSTR_EQUAL;
}
struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE) == CSTR_LESS_THAN; }
};
const ConfiguredTask& find(const std::vector<ConfiguredTask>& rows, const std::string& id) {
    const auto row = std::find_if(rows.begin(),rows.end(),[&](const auto& item) { return item.task.id == id; });
    if (row == rows.end()) throw std::invalid_argument("Configured task does not exist");
    return *row;
}
bool ownsOutput(TaskState state) { return state == TaskState::queued || state == TaskState::running || state == TaskState::paused || state == TaskState::interrupted; }
}
TaskQueue::TaskQueue(const std::filesystem::path& database)
    : owner_(std::make_unique<QueueOwner>(database)), store_(utf8(database)) { recovered_ = store_.recoverConfigured(); }
TaskQueue::~TaskQueue() { stop(); if (worker_.joinable()) worker_.join(); }
void TaskQueue::stop() { std::stop_source source; { std::lock_guard lock(activity_); source = runCancel_; } source.request_stop(); }
void TaskQueue::wait() { if (worker_.joinable()) worker_.join(); }
std::vector<std::string> TaskQueue::enqueue(std::vector<ProcessingJob> jobs, std::stop_token stop) {
    std::lock_guard lock(activity_);
    const auto current = store_.configured();
    std::set<std::wstring,PathLess> outputs;
    for (const auto& row : current) if (ownsOutput(row.task.state)) outputs.insert(std::filesystem::weakly_canonical(std::filesystem::absolute(path(row.task.outputPath))).wstring());
    std::vector<NewConfiguredTask> batch;
    for (auto& job : jobs) {
        if (stop.stop_requested()) throw std::runtime_error("Task import cancelled");
        if (job.source.empty() || job.output.empty() || job.modelDirectory.empty() || job.runtimeDirectory.empty())
            throw std::invalid_argument("Source, output, model and runtime paths are required");
        job.source = utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(path(job.source))));
        job.output = utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(path(job.output))));
        job.modelDirectory = std::filesystem::weakly_canonical(std::filesystem::absolute(job.modelDirectory));
        job.runtimeDirectory = std::filesystem::weakly_canonical(std::filesystem::absolute(job.runtimeDirectory));
        if (!std::filesystem::is_regular_file(path(job.source)) || std::filesystem::exists(path(job.output)) || !std::filesystem::is_directory(path(job.output).parent_path()))
            throw std::invalid_argument("Source must exist and output must be a new file in an existing local directory");
        if (!outputs.insert(path(job.output).wstring()).second) throw std::invalid_argument("Another pending/interrupted task or this batch owns this output path");
        pinModelDescriptor(job);
        batch.push_back({job.source,job.output,serializeJob(job)});
    }
    return store_.createConfigured(batch,stop);
}
bool TaskQueue::start(Callback callback) {
    if (!callback) throw std::invalid_argument("Queue callback is required");
    std::stop_token token;
    {
        std::lock_guard lock(activity_);
        if (busy_.load()) return false;
        runCancel_ = std::stop_source(); token = runCancel_.get_token(); busy_.store(true);
    }
    try {
        if (worker_.joinable()) worker_.join();
        pause_.resume();
        worker_ = std::jthread([this, callback = std::move(callback), token] {
            std::string error;
            try { run(token,callback); } catch (const std::exception& failure) { error = failure.what(); }
            { std::lock_guard lock(activity_); activeId_.clear(); }
            busy_.store(false);
            QueueEvent event; event.drained = true; event.queueError = std::move(error); callback(std::move(event));
        });
    } catch (...) { busy_.store(false); throw; }
    return true;
}
void TaskQueue::cancel(const std::string& id) {
    std::stop_source cancel;
    {
        std::lock_guard lock(activity_);
        if (activeId_ != id) { store_.transition(id,TaskState::cancelled); return; }
        cancel = currentCancel_;
    }
    cancel.request_stop();
}
void TaskQueue::retry(const std::string& id) {
    std::lock_guard lock(activity_);
    const auto rows = store_.configured(); const auto& task = find(rows,id);
    if (task.task.state != TaskState::failed) throw std::logic_error("Only failed tasks may be retried; interrupted tasks require an explicit restart");
    if (std::filesystem::exists(path(task.task.outputPath))) throw std::invalid_argument("Retry output already exists; keep it and create a new output task");
    for (const auto& other : rows) if (other.task.id != id && ownsOutput(other.task.state) && samePath(other.task.outputPath,task.task.outputPath)) throw std::logic_error("Another task owns the retry output");
    store_.transition(id,TaskState::queued);
}
std::string TaskQueue::restart(const std::string& id, const std::string& output) {
    const auto rows = store_.configured(); const auto& old = find(rows,id);
    if (old.task.state != TaskState::interrupted) throw std::logic_error("Restart-from-zero applies only to interrupted tasks");
    if (samePath(old.task.outputPath,output)) throw std::invalid_argument("Interrupted restart requires a new output path");
    auto job = parseJob(old.task.sourcePath,output,old.configuration); applyStoredIdentity(job,old.identity);
    return enqueue({std::move(job)}).front();
}
void TaskQueue::run(std::stop_token stop, const Callback& callback) {
    while (!stop.stop_requested()) {
        std::optional<ConfiguredTask> current;
        std::stop_source cancellation;
        {
            std::lock_guard lock(activity_);
            const auto tasks = store_.configured();
            const auto next = std::find_if(tasks.begin(),tasks.end(),[](const auto& row) { return row.task.state == TaskState::queued; });
            if (next == tasks.end()) return;
            current = *next; currentCancel_ = cancellation; activeId_ = next->task.id;
            store_.transition(activeId_,TaskState::running);
        }
        std::stop_callback shutdown(stop,[cancellation]() mutable { cancellation.request_stop(); });
        const auto started = std::chrono::steady_clock::now();
        auto lastSaved = started;
        ProcessingEvent lastProgress;
        ProcessingEvent terminal;
        try {
            auto job = parseJob(current->task.sourcePath,current->task.outputPath,current->configuration);
            applyStoredIdentity(job,current->identity);
            auto result = executeProcessingJob(job,[&](ProcessingEvent event) {
                const auto now = std::chrono::steady_clock::now();
                if (event.kind == ProcessingEvent::Kind::prepared && event.identity) store_.saveIdentity(current->task.id,serializeIdentity(*event.identity));
                if (event.stage == "paused" || event.stage == "resumed") {
                    store_.transition(current->task.id,event.stage == "paused" ? TaskState::paused : TaskState::running);
                    event.outputFrames = lastProgress.outputFrames; event.aiFrames = lastProgress.aiFrames;
                    event.timestamp = lastProgress.timestamp; event.processingFps = lastProgress.processingFps;
                }
                if (now - lastSaved >= std::chrono::seconds(1) || event.stage == "paused" || event.stage == "resumed" || event.kind == ProcessingEvent::Kind::prepared) {
                    store_.saveProgress(current->task.id,serializeProgress(event)); lastSaved = now;
                }
                lastProgress = event;
                callback({current->task.id,std::move(event),false,{}});
            },cancellation.get_token(),pause_);
            result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            result.processingFps = static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds;
            store_.finish(current->task.id,TaskState::completed,serializeResult(result));
            terminal.kind = ProcessingEvent::Kind::completed; terminal.result = std::move(result);
        } catch (const std::exception& error) {
            terminal.kind = cancellation.stop_requested() ? ProcessingEvent::Kind::cancelled : ProcessingEvent::Kind::failed;
            terminal.detail = error.what();
            store_.finish(current->task.id,cancellation.stop_requested() ? TaskState::cancelled : TaskState::failed,{},terminal.detail);
        }
        { std::lock_guard lock(activity_); activeId_.clear(); }
        callback({current->task.id,std::move(terminal),false,{}});
    }
}
} // namespace rifelove::application

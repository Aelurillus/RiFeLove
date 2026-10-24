#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace rifelove;
using namespace rifelove::application;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::filesystem::path path(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
std::string utf8(const std::filesystem::path& path) { const auto value = path.u8string(); return {value.begin(),value.end()}; }
bool hasWrittenBytes(const std::filesystem::path& path) {
    // Directory-entry sizes can remain stale while a Windows writer is open.
    // Query the live file object, as the crash test needs an encoded prefix.
    // https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-findfirstfileexw
    const auto file = CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{}; const bool ready = GetFileSizeEx(file,&length) && length.QuadPart > 0; CloseHandle(file); return ready;
}
struct Events {
    std::mutex mutex; std::condition_variable condition;
    bool paused = false, drained = false, waiting = false;
    std::string error;
    void waitFor(bool& flag) {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock,std::chrono::seconds(90),[&] { return flag; }),"Queue test timed out");
    }
};
ProcessingJob job(const std::string& source, const std::filesystem::path& output, const std::string& model, const std::string& runtime) {
    ProcessingJob result; result.source = source; result.output = utf8(output); result.modelDirectory = path(model); result.runtimeDirectory = path(runtime);
    result.settings.targetRate = Rational(60); result.settings.encoding.encoder = "ffv1"; return result;
}
}
int wmain(int argc, wchar_t** wide) {
    std::vector<std::string> values;
    for (int i = 0; i < argc; ++i) values.push_back(utf8(std::filesystem::path(wide[i])));
    std::vector<const char*> pointers;
    for (const auto& value : values) pointers.push_back(value.c_str());
    const auto argv = pointers.data();
    try {
        require(argc == 7,"Expected MODE DIRECTORY SOURCE PACK RUNTIME MARKER");
        const std::string mode = argv[1]; const auto root = path(argv[2]);
        const auto database = root / "native-queue.sqlite";
        TaskQueue queue(database);
        if (mode == "owner") { std::cout << "ownership acquired\n"; return 0; }
        if (mode == "stress") {
            std::vector<ProcessingJob> jobs;
            for (int index = 0; index < 20; ++index) {
                auto task = job(argv[3],root / ("stress-" + std::to_string(index) + ".mkv"),argv[4],argv[5]);
                task.settings.targetRate = Rational(10); jobs.push_back(std::move(task));
            }
            (void)queue.enqueue(std::move(jobs)); Events events;
            require(queue.start([&](QueueEvent event) { if (event.drained) { std::lock_guard lock(events.mutex); events.error = event.queueError; events.drained = true; events.condition.notify_one(); } }),"Repeated-session queue did not start");
            events.waitFor(events.drained); queue.wait(); require(events.error.empty(),"Repeated-session queue aborted");
            const auto rows = queue.list(); require(rows.size() == 20,"Repeated-session batch was not persisted");
            for (const auto& row : rows) require(row.task.state == TaskState::completed,"Repeated-session task failed");
            std::ofstream report(path(argv[6])); report << "{\"success\":true,\"sequentialSessions\":20}\n"; return 0;
        }
        if (mode == "recover") {
            require(queue.recoveredCount() == 1,"Crash did not recover exactly one configured task");
            const auto rows = queue.list(); require(rows.size() == 1 && rows[0].task.state == TaskState::interrupted,"Crash task is not interrupted");
            bool rejected = false; try { queue.retry(rows[0].task.id); } catch (const std::logic_error&) { rejected = true; }
            require(rejected,"Interrupted task was incorrectly treated as a retry/checkpoint");
            const auto id = queue.restart(rows[0].task.id,utf8(root / "restarted-from-zero.mkv"));
            Events events;
            require(queue.start([&](QueueEvent event) { if (event.drained) { std::lock_guard lock(events.mutex); events.error = event.queueError; events.drained = true; events.condition.notify_one(); } }),"Recovery queue did not start");
            events.waitFor(events.drained); queue.wait(); require(events.error.empty(),"Recovery queue errored");
            const auto after = queue.list(); require(after.size() == 2 && after[0].task.state == TaskState::interrupted && after[1].task.id == id && after[1].task.state == TaskState::completed,"Restart failed or rewrote interrupted history");
            return 0;
        }
        if (mode == "hold") {
            auto task = job(argv[3],root / "crash-output.mkv",argv[4],argv[5]);
            const auto id = queue.enqueue({task}).front();
            Events events;
            require(queue.start([&](QueueEvent event) {
                if (event.processing.stage == "processing" && event.processing.outputFrames >= 20) {
                    // Matroska buffers a cluster. Wait for a real on-disk
                    // encoded prefix rather than killing an empty reservation.
                    for (const auto& file : std::filesystem::directory_iterator(root)) {
                        if (file.path().extension() == ".partial" && hasWrittenBytes(file.path())) { queue.pause(); break; }
                    }
                }
                if (event.processing.stage == "paused") {
                    std::ofstream marker(path(argv[6])); marker << id; marker.close();
                    std::lock_guard lock(events.mutex); events.paused = true; events.condition.notify_one();
                }
                if (event.drained) { std::lock_guard lock(events.mutex); events.drained = true; events.condition.notify_one(); }
            }),"Hold queue did not start");
            events.waitFor(events.paused);
            // The harness terminates this process while a real writer and
            // source lease are open, exercising kernel lock release and WAL.
            events.waitFor(events.drained); queue.wait(); return 0;
        }
        require(mode == "basic","Unknown workflow mode");
        auto first = job(argv[3],root / "first.mkv",argv[4],argv[5]);
        auto second = first; second.output = utf8(root / "second.mkv");
        auto third = first; third.output = utf8(root / "third.mkv");
        auto fourth = first; fourth.output = utf8(root / "cancelled-queued.mkv");
        const auto ids = queue.enqueue({first,second,third,fourth});
        queue.cancel(ids[3]);
        { std::ofstream sentinel(path(second.output)); sentinel << "keep this file"; }
        Events events;
        require(queue.start([&](QueueEvent event) {
            if (event.taskId == ids[0] && event.processing.kind == ProcessingEvent::Kind::prepared) queue.pause();
            if (event.processing.stage == "paused") { std::lock_guard lock(events.mutex); events.paused = true; events.condition.notify_one(); }
            if (event.drained) { std::lock_guard lock(events.mutex); events.error = event.queueError; events.drained = true; events.condition.notify_one(); }
        }),"Batch did not start");
        events.waitFor(events.paused);
        require(queue.list()[0].task.state == TaskState::paused,"Pause acknowledged before durable state update");
        const auto handle = CreateFileW(path(argv[3]).c_str(),GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        require(handle == INVALID_HANDLE_VALUE,"Active source was writable while the read lease was held");
        bool locked = false; try { TaskQueue duplicate(database); } catch (const std::exception&) { locked = true; }
        require(locked,"Two queue owners were permitted");
        {
            TaskQueue competing(root / "waiting-queue.sqlite");
            auto waitingJob = first; waitingJob.output = utf8(root / "cancelled-waiting.mkv");
            (void)competing.enqueue({waitingJob}); Events waiting;
            require(competing.start([&](QueueEvent event) {
                if (event.processing.stage == "waiting_for_engine") { std::lock_guard lock(waiting.mutex); waiting.waiting = true; waiting.condition.notify_one(); }
                if (event.drained) { std::lock_guard lock(waiting.mutex); waiting.drained = true; waiting.condition.notify_one(); }
            }),"Second queue did not start waiting");
            waiting.waitFor(waiting.waiting); competing.stop(); waiting.waitFor(waiting.drained); competing.wait();
            require(competing.list()[0].task.state == TaskState::cancelled,"Waiting cancellation failed");
        }
        queue.resume(); events.waitFor(events.drained); queue.wait();
        require(events.error.empty(),"Queue aborted instead of isolating a task failure");
        auto rows = queue.list();
        require(rows.size() == 4 && rows[0].task.state == TaskState::completed && rows[1].task.state == TaskState::failed && rows[2].task.state == TaskState::completed && rows[3].task.state == TaskState::cancelled,"Wrong ordered batch terminal states");
        require(!rows[0].identity.empty() && !rows[0].result.empty(),"Identity/result evidence was not persisted");
        { std::ifstream sentinel(path(second.output)); std::string value; std::getline(sentinel,value); require(value == "keep this file","Failed task overwrote its output"); }
        std::filesystem::remove(path(second.output)); queue.retry(ids[1]);
        Events retry;
        require(queue.start([&](QueueEvent event) { if (event.drained) { std::lock_guard lock(retry.mutex); retry.error = event.queueError; retry.drained = true; retry.condition.notify_one(); } }),"Retry did not start");
        retry.waitFor(retry.drained); queue.wait();
        require(retry.error.empty() && queue.list()[1].task.state == TaskState::completed,"Failed retry did not complete");
        // Pause then cancel an active job. The next pending task stays queued
        // when the whole runner is stopped, rather than being discarded.
        auto cancelled = first; cancelled.output = utf8(root / "cancelled-active.mkv");
        auto pending = first; pending.output = utf8(root / "pending.mkv");
        const auto more = queue.enqueue({cancelled,pending}); Events stopped;
        require(queue.start([&](QueueEvent event) {
            if (event.taskId == more[0] && event.processing.kind == ProcessingEvent::Kind::prepared) queue.pause();
            if (event.processing.stage == "paused") { std::lock_guard lock(stopped.mutex); stopped.paused = true; stopped.condition.notify_one(); }
            if (event.drained) { std::lock_guard lock(stopped.mutex); stopped.error = event.queueError; stopped.drained = true; stopped.condition.notify_one(); }
        }),"Cancel run did not start");
        stopped.waitFor(stopped.paused); queue.stop(); stopped.waitFor(stopped.drained); queue.wait();
        rows = queue.list(); require(rows[4].task.state == TaskState::cancelled && rows[5].task.state == TaskState::queued,"Stop lost pending work or failed cancellation");
        require(!std::filesystem::exists(path(cancelled.output)),"Cancelled job published output");
        std::ofstream report(path(argv[6])); report << "{\"success\":true,\"pauseResume\":true,\"failedRetry\":true,\"sourceProtected\":true,\"singleOwner\":true,\"pendingPreserved\":true}\n";
        std::cout << "Native persistent queue workflow passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}

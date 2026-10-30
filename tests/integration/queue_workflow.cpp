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
        require(argc >= 3,"Expected MODE DIRECTORY SOURCE PACK RUNTIME MARKER [RESTORATION_PACK] [TEMPORAL_RESTORATION_PACK]");
        const std::string mode = argv[1]; const auto root = path(argv[2]);
        const bool dualRestorationHold = mode == "dual-restoration-hold";
        const bool dualRestorationRecovery = mode == "dual-restoration-recover";
        const bool restorationHold = mode == "restoration-hold";
        const bool restorationRecoveryMode = mode == "restoration-recover";
        const bool motionBlurHold = mode == "motion-blur-hold";
        const bool motionBlurRecovery = mode == "motion-blur-recover";
        require(dualRestorationHold ? argc == 9 : (restorationHold ? argc == 8 : argc == 7),
            "Expected MODE DIRECTORY SOURCE PACK RUNTIME MARKER [RESTORATION_PACK] [TEMPORAL_RESTORATION_PACK]");
        require((!dualRestorationRecovery && !motionBlurRecovery) || argc == 7,
            "Dual restoration recovery expects MODE DIRECTORY SOURCE PACK RUNTIME MARKER");
        const auto database = root / "native-queue.sqlite";
        const bool restorationRecovery = restorationHold || restorationRecoveryMode || dualRestorationHold || dualRestorationRecovery || motionBlurHold || motionBlurRecovery;
        TaskQueue queue(database,restorationRecovery ? 2 : (mode == "hold" || mode == "hold-complete" || mode == "hold-published" ||
            mode == "reject-published" || mode == "reject-proof" || mode == "recover") ? 16 : 256);
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
        if (mode == "reject-published") {
            const auto rows = queue.list();
            require(rows.size() == 1 && rows[0].task.state == TaskState::interrupted,"Published-output mismatch task was not interrupted");
            bool rejected = false;
            std::string rejectionReason;
            try { queue.resumeCheckpoint(rows[0].task.id); }
            catch (const std::invalid_argument& error) {
                rejectionReason = error.what();
                rejected = rejectionReason.find("does not match its pre-publication validation proof") != std::string::npos;
            }
            if (!rejected) throw std::runtime_error("Modified published output was accepted as a completed task; path=" +
                rows[0].task.outputPath + "; exists=" + std::to_string(std::filesystem::exists(path(rows[0].task.outputPath))) +
                "; reason=" + rejectionReason);
            return 0;
        }
        if (mode == "reject-proof") {
            const auto rows = queue.list();
            require(rows.size() == 1 && rows[0].task.state == TaskState::interrupted,
                "Invalid publication result was incorrectly adopted as completed");
            require(rows[0].task.error.find("Saved output validation result is invalid") != std::string::npos,
                "Invalid publication result did not produce a recovery diagnostic");
            return 0;
        }
        if (mode == "recover" || mode == "restoration-recover" || mode == "dual-restoration-recover" || mode == "motion-blur-recover") {
            require(queue.recoveredCount() <= 1,"Recovery changed an unexpected number of queue tasks");
            const auto rows = queue.list(); require(rows.size() == 1,"Crash task is missing");
            if (rows[0].task.state == TaskState::completed) {
                require(queue.recoveredPublishedOutputCount() == 1,"Published output was not reconciled during queue startup");
                require(std::filesystem::is_regular_file(path(rows[0].task.outputPath)),"Recovered published output is missing");
                return 0;
            }
            require(rows[0].task.state == TaskState::interrupted,"Crash task is neither interrupted nor a reconciled published output");
            bool rejected = false; try { queue.retry(rows[0].task.id); } catch (const std::logic_error&) { rejected = true; }
            require(rejected,"Interrupted task was incorrectly treated as a retry/checkpoint");
            bool resumedCheckpoint = false;
            try { queue.resumeCheckpoint(rows[0].task.id); resumedCheckpoint = true; } catch (const std::logic_error&) {}
            require(resumedCheckpoint,"Interrupted task with a committed segment could not be resumed");
            Events events;
            require(queue.start([&](QueueEvent event) { if (event.drained) { std::lock_guard lock(events.mutex); events.error = event.queueError; events.drained = true; events.condition.notify_one(); } }),"Recovery queue did not start");
            events.waitFor(events.drained); queue.wait(); require(events.error.empty(),"Checkpoint recovery queue errored");
            const auto after = queue.list(); require(after.size() == 1 && after[0].task.id == rows[0].task.id && after[0].task.state == TaskState::completed,"Checkpoint resume failed or rewrote task history");
            require(std::filesystem::is_regular_file(path(after[0].task.outputPath)),"Checkpoint resume did not publish the requested output");
            return 0;
        }
        if (mode == "hold" || mode == "hold-complete" || mode == "hold-published" || mode == "restoration-hold" || mode == "dual-restoration-hold" || mode == "motion-blur-hold") {
            auto task = job(argv[3],root / "crash-output.mkv",argv[4],argv[5]);
            task.settings.targetRate = (mode == "restoration-hold" || mode == "dual-restoration-hold" || mode == "motion-blur-hold") ? Rational(60) :
                (mode == "hold-complete" || mode == "hold-published" ? Rational(64,11) : Rational(6));
            if (mode == "restoration-hold" || mode == "dual-restoration-hold") {
                task.restorationModelDirectory = path(argv[7]);
                if (mode == "dual-restoration-hold") task.temporalRestorationModelDirectory = path(argv[8]);
                task.settings.inferenceBackend = "cuda";
            }
            if (mode == "motion-blur-hold") {
                task.settings.inferenceBackend = "cuda";
                task.settings.motionBlur.enabled = true;
                task.settings.motionBlur.shutterAngleDegrees = 180;
                task.settings.motionBlur.sampleCount = 4;
            }
            const auto id = queue.enqueue({task}).front();
            Events events;
            require(queue.start([&](QueueEvent event) {
                if (event.processing.stage == "processing") {
                    std::ofstream status(root / "hold-progress.txt"); status << event.processing.outputFrames;
                }
                if ((mode == "hold" || mode == "restoration-hold" || mode == "dual-restoration-hold" || mode == "motion-blur-hold") && event.processing.stage == "processing" &&
                    event.processing.outputFrames >= ((mode == "restoration-hold" || mode == "dual-restoration-hold" || mode == "motion-blur-hold") ? 4U : 18U)) {
                    // Frame 17 is reported only after the first 16-frame
                    // checkpoint is committed; frame 18 has opened the next
                    // live segment writer, so the crash tests both artifacts.
                    for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
                        const auto name = file.path().filename().wstring();
                        if (file.path().extension() == ".mkv" && name.starts_with(L"segment-")) { queue.pause(); break; }
                    }
                }
                if (mode == "hold-complete" && event.processing.stage == "validating")
                    queue.pause(); // All output segments are committed before final validation starts.
                if (mode == "hold-published" && event.processing.stage == "completed") {
                    std::ofstream marker(path(argv[6])); marker << id; marker.close();
                    while (!std::filesystem::exists(root / "release.txt")) Sleep(10);
                }
                if (event.processing.stage == "paused") {
                    std::ofstream marker(path(argv[6])); marker << id; marker.close();
                    std::lock_guard lock(events.mutex); events.paused = true; events.condition.notify_one();
                }
                if (event.drained) { std::lock_guard lock(events.mutex); events.error = event.queueError; events.drained = true; if (!events.paused) events.paused = true; events.condition.notify_one(); }
            }),"Hold queue did not start");
            events.waitFor(events.paused);
            if (!queue.list().empty() && queue.list()[0].task.state != TaskState::paused) {
                const auto row = queue.list()[0];
                throw std::runtime_error("Hold task failed before reaching its checkpoint: state=" + toString(row.task.state) +
                    "; error=" + row.task.error + "; queue=" + events.error);
            }
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
        if (rows.size() == 4 && !(rows[0].task.state == TaskState::completed && rows[1].task.state == TaskState::failed && rows[2].task.state == TaskState::completed && rows[3].task.state == TaskState::cancelled))
            for (const auto& row : rows) std::cerr << "task " << row.task.id << " state=" << toString(row.task.state) << " error=" << row.task.error << '\n';
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

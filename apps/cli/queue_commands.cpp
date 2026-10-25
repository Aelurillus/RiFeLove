#include "queue_commands.hpp"
#include "console_cancellation.hpp"
#include "application/task_queue.hpp"
#include "application/job_serialization.hpp"
#include "inference/model_pack.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <iostream>

namespace {
using namespace rifelove;
using namespace rifelove::application;
using Json = nlohmann::json;
std::filesystem::path path(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
template<class T> T number(const std::string& value) {
    T result{}; const auto [end,error] = std::from_chars(value.data(),value.data() + value.size(),result);
    if (error != std::errc{} || end != value.data() + value.size()) throw std::invalid_argument("Invalid numeric option"); return result;
}
void emit(const Json& value) { std::cout << value.dump() << '\n' << std::flush; }
Json parsed(const std::string& value) { return value.empty() ? Json(nullptr) : Json::parse(value); }
}
int runQueueCommand(const std::vector<std::string>& args) {
    using namespace rifelove; using namespace rifelove::application;
    if (args.size() < 3) throw std::invalid_argument("Queue command requires DATABASE");
    const auto& command = args[1];
    if (command != "queue-list" && command != "queue-export" && command != "queue-add" && command != "queue-import" && command != "queue-run" && command != "queue-retry" && command != "queue-cancel" && command != "queue-restart" && command != "queue-resume")
        throw std::invalid_argument("Unknown queue command");
    if (command == "queue-list" || command == "queue-export") {
        if (args.size() != 3) throw std::invalid_argument("Read-only queue command accepts DATABASE only");
        TaskStore store(args[2],TaskStore::OpenMode::readOnly);
        Json jobs = Json::array();
        for (const auto& row : store.configured()) {
            if (command == "queue-list") emit({{"id",row.task.id},{"source",row.task.sourcePath},{"output",row.task.outputPath},{"state",toString(row.task.state)},
                {"error",row.task.error},{"configuration",parsed(row.configuration)},{"identity",parsed(row.identity)},{"progress",parsed(row.progress)},{"result",parsed(row.result)}});
            else {
                auto job = parseJob(row.task.sourcePath,row.task.outputPath,row.configuration); applyStoredIdentity(job,row.identity);
                jobs.push_back({{"source",row.task.sourcePath},{"output",row.task.outputPath},{"configuration",Json::parse(serializeJob(job))}});
            }
        }
        if (command == "queue-export") emit({{"schemaVersion",1},{"jobs",std::move(jobs)}});
        return 0;
    }
    if (command != "queue-add" && command != "queue-import" && !std::filesystem::is_regular_file(path(args[2]))) throw std::invalid_argument("Queue database does not exist");
    TaskQueue queue(path(args[2]));
    if (command == "queue-add") {
        if (args.size() < 6) throw std::invalid_argument("queue-add DATABASE INPUT OUTPUT --fps RATE --model-dir PACK --runtime-dir DLL_DIRECTORY");
        ProcessingJob job; job.source = args[3]; job.output = args[4]; bool hasRate = false;
        for (std::size_t index = 5; index < args.size(); ++index) {
            const auto& option = args[index];
            if (++index >= args.size()) throw std::invalid_argument("Missing queue option value");
            const auto& value = args[index];
            if (option == "--fps") { job.settings.targetRate = Rational::parse(value); hasRate = true; }
            else if (option == "--model-dir") job.modelDirectory = path(value);
            else if (option == "--runtime-dir") job.runtimeDirectory = path(value);
            else if (option == "--encoder") job.settings.encoding.encoder = value;
            else if (option == "--pixel-format") job.settings.encoding.pixelFormat = value;
            else if (option == "--bitrate") job.settings.encoding.bitRate = number<std::int64_t>(value);
            else if (option == "--bframes") job.settings.encoding.maxBFrames = number<int>(value);
            else if (option == "--gpu") job.deviceOrdinal = number<int>(value);
            else if (option == "--stream") job.settings.encoding.videoStreamIndex = number<int>(value);
            else if (option == "--cut-threshold") job.settings.scene.cutMeanDifference = number<double>(value);
            else if (option == "--option") { const auto at = value.find('='); if (at == std::string::npos || at == 0) throw std::invalid_argument("Encoder option requires KEY=VALUE"); job.settings.encoding.encoderOptions[value.substr(0,at)] = value.substr(at + 1); }
            else throw std::invalid_argument("Unknown queue option: " + option);
        }
        if (!hasRate || job.modelDirectory.empty() || job.runtimeDirectory.empty()) throw std::invalid_argument("FPS, model pack and runtime directory are required");
        emit({{"event","queued"},{"ids",queue.enqueue({std::move(job)})}}); return 0;
    }
    if (command == "queue-import") {
        if (args.size() != 4) throw std::invalid_argument("queue-import DATABASE PLAN_JSON");
        const auto bytes = inference::readLocalFile(path(args[3]),16 * 1024 * 1024);
        if (bytes.empty()) throw std::invalid_argument("Queue import is empty");
        const auto plan = Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        if (plan.at("schemaVersion") != 1 || !plan.at("jobs").is_array() || plan.at("jobs").empty() || plan.at("jobs").size() > 10000) throw std::invalid_argument("Invalid queue import schema/batch size");
        std::vector<ProcessingJob> jobs;
        for (const auto& row : plan.at("jobs")) jobs.push_back(parseJob(row.at("source").get<std::string>(),row.at("output").get<std::string>(),row.at("configuration").dump()));
        emit({{"event","queued"},{"ids",queue.enqueue(std::move(jobs))}}); return 0;
    }
    if (command == "queue-retry" || command == "queue-cancel") {
        if (args.size() != 4) throw std::invalid_argument("Queue action requires DATABASE ID");
        if (command == "queue-retry") queue.retry(args[3]); else queue.cancel(args[3]);
        emit({{"event",command == "queue-retry" ? "retry_queued" : "task_cancelled"},{"id",args[3]}}); return 0;
    }
    if (command == "queue-restart") {
        if (args.size() != 5) throw std::invalid_argument("queue-restart DATABASE INTERRUPTED_ID NEW_OUTPUT");
        emit({{"event","restart_from_zero_queued"},{"id",queue.restart(args[3],args[4])}}); return 0;
    }
    if (command == "queue-resume") {
        if (args.size() != 4) throw std::invalid_argument("queue-resume DATABASE INTERRUPTED_ID");
        queue.resumeCheckpoint(args[3]);
        emit({{"event","checkpoint_resume_queued"},{"id",args[3]}}); return 0;
    }
    if (command != "queue-run" || args.size() > 4 || (args.size() == 4 && args[3] != "--progress")) throw std::invalid_argument("Unknown or invalid queue command");
    const bool showProgress = args.size() == 4;
    cli::ConsoleCancellation cancellation;
    std::stop_callback stop(cancellation.token(),[&] { queue.stop(); });
    std::mutex mutex; std::condition_variable condition; bool done = false; std::string queueError;
    emit({{"event","queue_started"},{"recovered_interrupted",queue.recoveredCount()}});
    if (!queue.start([&](QueueEvent event) {
        if (event.drained) {
            { std::lock_guard lock(mutex); queueError = event.queueError; done = true; }
            condition.notify_one(); return;
        }
        const auto& p = event.processing;
        if (p.kind == ProcessingEvent::Kind::completed) emit({{"event","task_completed"},{"id",event.taskId},{"result",Json::parse(serializeResult(*p.result))}});
        else if (p.kind == ProcessingEvent::Kind::failed || p.kind == ProcessingEvent::Kind::cancelled) emit({{"event",p.kind == ProcessingEvent::Kind::failed ? "task_failed" : "task_cancelled"},{"id",event.taskId},{"error",p.detail}});
        else if (showProgress) emit({{"event","task_progress"},{"id",event.taskId},{"progress",Json::parse(serializeProgress(p))}});
    })) throw std::runtime_error("Queue is already running");
    if (cancellation.token().stop_requested()) queue.stop();
    { std::unique_lock lock(mutex); condition.wait(lock,[&] { return done; }); }
    queue.wait();
    std::size_t failed = 0, interrupted = 0, completed = 0, queued = 0;
    for (const auto& row : queue.list()) {
        failed += row.task.state == TaskState::failed; interrupted += row.task.state == TaskState::interrupted;
        completed += row.task.state == TaskState::completed; queued += row.task.state == TaskState::queued;
    }
    emit({{"event","queue_stopped"},{"completed",completed},{"failed",failed},{"interrupted",interrupted},{"queued",queued},{"error",queueError}});
    if (cancellation.token().stop_requested()) return 130;
    return !queueError.empty() || failed ? 2 : interrupted ? 3 : 0;
}

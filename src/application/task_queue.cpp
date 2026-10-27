#include "application/task_queue.hpp"
#include "common/logging.hpp"
#include "application/checkpoint_artifacts.hpp"
#include "application/job_serialization.hpp"
#include "application/local_lease.hpp"
#include "media/media_engine.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::application {
namespace {
using Json = nlohmann::json;
constexpr std::size_t defaultCheckpointFramesPerSegment = 256;
std::string utf8(const std::filesystem::path& path) { const auto value = path.u8string(); return {value.begin(),value.end()}; }
std::filesystem::path path(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
bool samePath(const std::string& a, const std::string& b) {
    const auto left = std::filesystem::weakly_canonical(std::filesystem::absolute(path(a))).wstring();
    const auto right = std::filesystem::weakly_canonical(std::filesystem::absolute(path(b))).wstring();
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE) == CSTR_EQUAL;
}
bool pathEntryExists(const std::filesystem::path& value) {
    const auto attributes = GetFileAttributesW(value.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("Could not inspect task output path (Windows error " + std::to_string(error) + ")");
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
std::string checkpointManifest(const ProcessingJob& source, const std::string& identityText, std::size_t framesPerSegment,
    const std::optional<restoration::Capabilities>& restorationRuntime = std::nullopt) {
    auto identity = Json::parse(identityText);
    if (identity.value("schemaVersion",0) != 1 && identity.value("schemaVersion",0) != 2)
        throw std::invalid_argument("Unsupported task identity version for checkpoint resume");
    auto job = source;
    applyStoredIdentity(job,identityText);
    if (identity.value("schemaVersion",0) == 1) {
        identity["model"]["adapter"] = {{"id",job.expectedModelAdapterId},{"version",job.expectedModelAdapterVersion}};
        identity["schemaVersion"] = 2;
    }
    Json manifest{{"schemaVersion",2},{"source",job.source},{"output",job.output},
        {"job",Json::parse(serializeJob(job))},{"identity",identity},
        {"mediaRuntime",media::MediaEngine::runtimeVersion()},
        {"framesPerSegment",framesPerSegment}};
    if (restorationRuntime) {
        const auto& caps = *restorationRuntime;
        if (job.restorationModelDirectory.empty() || caps.modelId.empty() || caps.runtimeBackend.empty() || caps.runtimeSignature.empty())
            throw std::invalid_argument("Restoration checkpoint identity is incomplete");
        manifest["restorationRuntime"] = {{"modelId",caps.modelId},{"adapterId",caps.adapterId},
            {"adapterVersion",caps.adapterVersion},{"backend",caps.runtimeBackend},{"signature",caps.runtimeSignature},
            {"scaleNumerator",caps.scaleNumerator},{"scaleDenominator",caps.scaleDenominator}};
    }
    return manifest.dump();
}
void validatePublishedResult(const std::string& serialized) {
    const auto result = Json::parse(serialized);
    if (!result.is_object() || result.value("schemaVersion",0) != 1)
        throw std::invalid_argument("Saved publication result has an unsupported schema");
    const auto frames = result.at("outputFrames").get<std::uint64_t>();
    const auto aiFrames = result.at("aiFrames").get<std::uint64_t>();
    const auto originalFrames = result.at("originalFrames").get<std::uint64_t>();
    const auto heldFrames = result.at("heldFrames").get<std::uint64_t>();
    const auto decodedFrames = result.at("validation").at("decodedFrames").get<std::uint64_t>();
    if (frames == 0 || decodedFrames != frames || aiFrames > frames ||
        originalFrames > frames - aiFrames || heldFrames != frames - aiFrames - originalFrames)
        throw std::invalid_argument("Saved publication result does not match its validated frame counts");
}
}
TaskQueue::TaskQueue(const std::filesystem::path& database, std::size_t checkpointFramesPerSegment)
    : owner_(std::make_unique<QueueOwner>(database)), store_(utf8(database)),checkpointFramesPerSegment_(checkpointFramesPerSegment) {
    if (checkpointFramesPerSegment_ == 0) throw std::invalid_argument("Checkpoint segment size must be positive");
    (void)store_.recoverConfigured();
    reconcilePublishedOutputs();
    const auto tasks = store_.configured();
    recovered_ = static_cast<std::size_t>(std::count_if(tasks.begin(),tasks.end(),[](const auto& row) {
        return row.task.state == TaskState::interrupted;
    }));
}
void TaskQueue::reconcilePublishedOutputs() {
    for (const auto& row : store_.configured()) {
        if (row.task.state != TaskState::interrupted) continue;
        try {
            const auto publication = store_.outputPublicationIntent(row.task.id);
            if (!publication) continue;
            const auto output = path(row.task.outputPath);
            if (!pathEntryExists(output)) continue;
            try {
                SourceLease lease(output);
                const auto actual = lease.fingerprint(std::stop_token{});
                if (actual.size != publication->byteCount || actual.sha256 != publication->sha256) {
                    store_.setInterruptedRecoveryError(row.task.id,
                        "Published output does not match the saved validation proof; it was left untouched. Resume or restart using a new output path.");
                    continue;
                }
                try {
                    validatePublishedResult(publication->result);
                } catch (const std::exception& error) {
                    store_.setInterruptedRecoveryError(row.task.id,
                        "Saved output validation result is invalid; the published file was left untouched: " + std::string(error.what()));
                    continue;
                }
                store_.completePublishedOutput(row.task.id);
                ++recoveredPublishedOutputs_;
            } catch (const std::exception& error) {
                store_.setInterruptedRecoveryError(row.task.id,
                    "Could not verify and reconcile the published output after restart; it was left untouched: " + std::string(error.what()));
            }
        } catch (const std::exception& error) {
            store_.setInterruptedRecoveryError(row.task.id,
                "Could not reconcile the published output after restart; it was left untouched: " + std::string(error.what()));
        }
    }
}
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
        if (!job.restorationModelDirectory.empty())
            job.restorationModelDirectory = std::filesystem::weakly_canonical(std::filesystem::absolute(job.restorationModelDirectory));
        if (!std::filesystem::is_regular_file(path(job.source)) || std::filesystem::exists(path(job.output)) || !std::filesystem::is_directory(path(job.output).parent_path()))
            throw std::invalid_argument("Source must exist and output must be a new file in an existing local directory");
        if (!outputs.insert(path(job.output).wstring()).second) throw std::invalid_argument("Another pending/interrupted task or this batch owns this output path");
        pinModelDescriptor(job);
        pinRestorationModelDescriptor(job);
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
void TaskQueue::resumeCheckpoint(const std::string& id) {
    std::lock_guard lock(activity_);
    const auto rows = store_.configured(); const auto& task = find(rows,id);
    if (task.task.state != TaskState::interrupted) throw std::logic_error("Only interrupted tasks can resume from a checkpoint");
    const auto outputPath = path(task.task.outputPath);
    const bool outputExists = pathEntryExists(outputPath);
    for (const auto& other : rows) if (other.task.id != id && ownsOutput(other.task.state) && samePath(other.task.outputPath,task.task.outputPath))
        throw std::logic_error("Another task owns the interrupted output");
    if (task.identity.empty()) throw std::logic_error("This task has no saved processing identity; restart it with a new output");
    const auto manifest = store_.checkpointManifest(id);
    if (manifest.empty()) throw std::logic_error("This task has no verified checkpoint segments; restart it with a new output");
    auto job = parseJob(task.task.sourcePath,task.task.outputPath,task.configuration);
    applyStoredIdentity(job,task.identity);
    auto expectedManifest = Json::parse(checkpointManifest(job,task.identity,checkpointFramesPerSegment_));
    if (!job.restorationModelDirectory.empty()) {
        const auto saved = Json::parse(manifest);
        if (!saved.contains("restorationRuntime") || !saved.at("restorationRuntime").is_object() ||
            saved.at("restorationRuntime").value("modelId",std::string()).empty() ||
            saved.at("restorationRuntime").value("backend",std::string()).empty() ||
            saved.at("restorationRuntime").value("signature",std::string()).empty())
            throw std::logic_error("This checkpoint has no verified restoration runtime identity; restart with a new output");
        // The selected runtime is checked again after the model is loaded by
        // the worker, before any saved segment can be reused.
        expectedManifest["restorationRuntime"] = saved.at("restorationRuntime");
    }
    if (!CheckpointArtifacts::manifestMatches(manifest,expectedManifest.dump()))
        throw std::logic_error("Task settings, model identity or local media runtime changed; restart with a new output");
    CheckpointArtifacts artifacts(store_,id,path(task.task.outputPath));
    const auto segments = artifacts.verifyCommitted();
    if (segments.empty()) throw std::logic_error("This task has no complete verified checkpoint segments; restart it with a new output");
    if (std::any_of(segments.begin(),segments.end(),[](const media::VideoSegmentInput& segment) { return segment.contract.encoder.empty(); }))
        throw std::logic_error("Checkpoint encoder identity is missing; restart with a new output");
    if (outputExists) {
        const auto publication = store_.outputPublicationIntent(id);
        if (!publication)
            throw std::invalid_argument("Output already exists without a saved validation proof; keep it and restart with a new output path");
        SourceLease lease(outputPath);
        const auto actual = lease.fingerprint(std::stop_token{});
        if (actual.size != publication->byteCount || actual.sha256 != publication->sha256)
            throw std::invalid_argument("Existing output does not match its pre-publication validation proof; keep it and restart with a new output path");
        validatePublishedResult(publication->result);
        const auto expectedFrames = Json::parse(publication->result).at("outputFrames").get<std::uint64_t>();
        std::uint64_t committedFrames = 0;
        for (const auto& segment : segments) {
            if (segment.contract.frameCount > std::numeric_limits<std::uint64_t>::max() - committedFrames)
                throw std::runtime_error("Verified checkpoint frame count exceeds the supported range");
            committedFrames += static_cast<std::uint64_t>(segment.contract.frameCount);
        }
        if (expectedFrames == 0 || expectedFrames != committedFrames)
            throw std::runtime_error("Saved output validation result does not match the verified checkpoint frame count");
        store_.completePublishedOutput(id);
        return;
    }
    (void)artifacts.removeUncommittedSegments();
    store_.resumeCheckpointed(id);
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
            CheckpointArtifacts artifacts(store_,current->task.id,path(current->task.outputPath));
            const auto configurePipeline = [&](const ProcessingIdentity& identity, PipelineSettings& settings) {
                const auto identityJson = serializeIdentity(identity);
                std::optional<restoration::Capabilities> restorationRuntime;
                if (settings.restorationBackend) restorationRuntime = settings.restorationBackend->capabilities();
                if (job.restorationModelDirectory.empty() == restorationRuntime.has_value())
                    throw std::logic_error("Restoration model and runtime identity do not match");
                const auto manifest = checkpointManifest(job,identityJson,checkpointFramesPerSegment_,restorationRuntime);
                const auto previousManifest = artifacts.manifest();
                if (!previousManifest.empty() && !CheckpointArtifacts::manifestMatches(previousManifest,manifest))
                    throw std::logic_error("Checkpoint identity or runtime changed; refusing to reuse prior segments");
                store_.saveIdentity(current->task.id,identityJson);
                if (previousManifest.empty()) artifacts.setManifest(manifest);
                PipelineCheckpoint checkpoint;
                checkpoint.framesPerSegment = checkpointFramesPerSegment_;
                checkpoint.restorationIdentityVerified = restorationRuntime.has_value();
                checkpoint.completedSegments = artifacts.verifyCommitted(cancellation.get_token());
                checkpoint.segmentPath = [&artifacts](std::uint64_t index) { return utf8(artifacts.segmentPath(index)); };
                checkpoint.commitSegment = [&artifacts,token = cancellation.get_token()](std::uint64_t index,std::uint64_t first,
                    std::uint64_t end,const media::VideoSegmentInput& segment) {
                    (void)artifacts.commit(index,first,end,segment,token);
                };
                checkpoint.prepareOutputPublication = [this,id = current->task.id,token = cancellation.get_token(),
                    &started,&lastSaved,&lastProgress,&callback](const std::string& temporary, const PipelineResult& result) {
                    FileIdentity identity;
                    auto lastFingerprintUpdate = std::chrono::steady_clock::time_point{};
                    {
                        SourceLease output(path(temporary));
                        identity = output.fingerprint(token,[&](std::uint64_t currentBytes,std::uint64_t totalBytes) {
                            const auto now = std::chrono::steady_clock::now();
                            if (currentBytes != totalBytes && lastFingerprintUpdate.time_since_epoch().count() != 0 &&
                                now - lastFingerprintUpdate < std::chrono::milliseconds(500)) return;
                            ProcessingEvent event;
                            event.stage = "verifying_output";
                            event.detail = std::to_string(currentBytes) + " / " + std::to_string(totalBytes) + " bytes";
                            event.outputFrames = result.video.framesSubmitted;
                            event.aiFrames = result.aiFrames;
                            event.processingFps = result.processingFps;
                            event.timings = result.timings;
                            store_.saveProgress(id,serializeProgress(event));
                            lastSaved = now;
                            lastProgress = event;
                            callback({id,std::move(event),false,{}});
                            lastFingerprintUpdate = now;
                        });
                    }
                    auto publicationResult = result;
                    publicationResult.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    publicationResult.processingFps = publicationResult.elapsedSeconds > 0
                        ? static_cast<double>(publicationResult.video.framesSubmitted) / publicationResult.elapsedSeconds : 0;
                    store_.saveOutputPublicationIntent(id,identity.sha256,identity.size,serializeResult(publicationResult));
                };
                settings.checkpoint = std::move(checkpoint);
            };
            auto result = executeProcessingJob(job,[&](ProcessingEvent event) {
                const auto now = std::chrono::steady_clock::now();
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
            },cancellation.get_token(),pause_,configurePipeline);
            result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            result.processingFps = static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds;
            store_.finish(current->task.id,TaskState::completed,serializeResult(result));
            logging::write(logging::Level::info,"queue","task.completed","Queued output passed media and timeline validation",
                {{"task_id",current->task.id},{"encoder",result.video.encoder},
                    {"output_frames",static_cast<std::uint64_t>(result.video.framesSubmitted)}});
            terminal.kind = ProcessingEvent::Kind::completed; terminal.result = std::move(result);
        } catch (const std::exception& error) {
            const bool cancelled = cancellation.stop_requested();
            logging::write(cancelled ? logging::Level::warning : logging::Level::error,"queue",
                cancelled ? "task.cancelled" : "task.failed",error.what(),{{"task_id",current->task.id}});
            terminal.kind = cancelled ? ProcessingEvent::Kind::cancelled : ProcessingEvent::Kind::failed;
            terminal.detail = error.what();
            store_.finish(current->task.id,cancelled ? TaskState::cancelled : TaskState::failed,{},terminal.detail);
        }
        { std::lock_guard lock(activity_); activeId_.clear(); }
        callback({current->task.id,std::move(terminal),false,{}});
    }
}
} // namespace rifelove::application

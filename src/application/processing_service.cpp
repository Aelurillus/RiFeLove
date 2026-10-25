#include "application/processing_service.hpp"
#include "common/logging.hpp"
#include "inference/onnx_cuda_backend.hpp"
#include "media/rife_color.hpp"
#include <algorithm>
#include <array>
#include <chrono>

namespace rifelove::application {
namespace {
std::filesystem::path localPath(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
std::timed_mutex engineSlot;
}
PipelineResult executeProcessingJob(const ProcessingJob& job, const std::function<void(ProcessingEvent)>& progress,
    std::stop_token stop, PauseGate& pause,
    const std::function<void(const ProcessingIdentity&,PipelineSettings&)>& configurePipeline) {
    const auto started = std::chrono::steady_clock::now();
    auto lastUpdate = started;
    auto lastProgressLog = started;
    std::string lastStage;
    auto send = [&](ProcessingEvent event) { if (progress) progress(std::move(event)); };
    auto checkpoint = [&] {
        pause.checkpoint(stop,[&](bool paused) {
            ProcessingEvent event; event.stage = paused ? "paused" : "resumed"; send(std::move(event));
        });
    };
    checkpoint();
    std::unique_lock<std::timed_mutex> execution(engineSlot,std::defer_lock);
    bool waiting = false;
    while (!execution.try_lock_for(std::chrono::milliseconds(50))) {
        checkpoint();
        if (!waiting) { ProcessingEvent event; event.stage = "waiting_for_engine"; send(std::move(event)); waiting = true; }
    }
    checkpoint();
    if (std::filesystem::exists(localPath(job.output))) throw std::invalid_argument("Output already exists; select a new path");
    SourceLease source(localPath(job.source));
    ProcessingEvent checking; checking.stage = "checking_source"; send(std::move(checking));
    auto identity = source.fingerprint(stop,[&](std::uint64_t current, std::uint64_t total) {
        checkpoint();
        const auto now = std::chrono::steady_clock::now();
        if (current != total && now - lastUpdate < std::chrono::milliseconds(100)) return;
        lastUpdate = now;
        ProcessingEvent event; event.stage = "checking_source"; event.detail = std::to_string(current) + " / " + std::to_string(total) + " bytes"; send(std::move(event));
    });
    if (!job.expectedSourceSha256.empty() && identity.sha256 != job.expectedSourceSha256)
        throw std::runtime_error("Source SHA-256 changed since this task was prepared; create a new task for changed media");
    checkpoint();
    ProcessingEvent loading; loading.stage = "loading_model"; send(std::move(loading));
    const auto pack = inference::loadModelPack(job.modelDirectory);
    if (!job.expectedModelSha256.empty() && pack.sha256 != job.expectedModelSha256)
        throw std::runtime_error("Model SHA-256 changed since this task was prepared; select a verified model for a new task");
    if ((!job.expectedModelId.empty() && pack.id != job.expectedModelId) || (!job.expectedModelVersion.empty() && pack.version != job.expectedModelVersion))
        throw std::runtime_error("Model identity/version changed since this task was queued");
    checkpoint();
    auto device = std::make_shared<gpu::CudaDevice>(job.deviceOrdinal);
    inference::OnnxCudaConfig config; config.runtimeDirectory = job.runtimeDirectory;
    inference::OnnxCudaBackend backend(device,config); backend.loadModel(pack);
    const auto capabilities = backend.capabilities();
    ProcessingIdentity processingIdentity{std::move(identity),pack.id,pack.version,pack.sha256,device->info(),capabilities.backend};
    ProcessingEvent prepared; prepared.kind = ProcessingEvent::Kind::prepared; prepared.stage = "loading_model";
    prepared.detail = capabilities.fallbackReason;
    prepared.identity = processingIdentity;
    send(std::move(prepared));
    auto pipelineSettings = job.settings;
    if (configurePipeline) configurePipeline(processingIdentity,pipelineSettings);
    checkpoint();
    return interpolateVideo(job.source,job.output,pipelineSettings,*device,backend,[&](const PipelineProgress& value) {
        // A completed progress event follows atomic output publication. Do
        // not acknowledge a new pause/cancel as though that output is partial.
        const auto now = std::chrono::steady_clock::now();
        if (value.stage == "processing" && now - lastProgressLog >= std::chrono::seconds(15)) {
            lastProgressLog = now;
            const double elapsedSeconds = std::chrono::duration<double>(now - started).count();
            const double processingFps = elapsedSeconds > 0 ? static_cast<double>(value.outputFrames) / elapsedSeconds : 0;
            logging::write(logging::Level::info,"processing","processing.progress",
                "Local video processing is continuing",
                {{"output_frames",static_cast<std::uint64_t>(value.outputFrames)},
                    {"ai_frames",static_cast<std::uint64_t>(value.aiFrames)},
                    {"processing_fps",processingFps},{"elapsed_seconds",elapsedSeconds},
                    {"encoder",value.encoder},{"inference_wait_seconds",value.timings.inferenceWaitSeconds},
                    {"frame_encode_seconds",value.timings.frameEncodeSeconds}});
        }
        if (value.stage != lastStage || now - lastUpdate >= std::chrono::milliseconds(100)) {
            lastUpdate = now; lastStage = value.stage;
            ProcessingEvent event; event.stage = value.stage; event.detail = value.detail; event.encoder = value.encoder;
            event.outputFrames = value.outputFrames; event.aiFrames = value.aiFrames; event.timestamp = value.timestamp;
            event.timings = value.timings;
            const auto seconds = std::chrono::duration<double>(now - started).count();
            event.processingFps = seconds > 0 ? static_cast<double>(value.outputFrames) / seconds : 0;
            send(std::move(event));
        }
        // Honor a pause/cancel requested by the observer at the just-reported
        // safe boundary. Check even when this progress event was throttled.
        if (value.stage != "completed") checkpoint();
    },stop);
}
ProcessingService::~ProcessingService() { cancel(); if (worker_.joinable()) worker_.join(); }
void ProcessingService::cancel() { if (worker_.joinable()) worker_.request_stop(); }
bool ProcessingService::launch(std::function<void(std::stop_token, const Callback&)> work, Callback callback) {
    if (!callback) throw std::invalid_argument("A processing callback is required");
    if (busy_.exchange(true)) return false;
    try {
        if (worker_.joinable()) worker_.join();
        pause_.resume();
        worker_ = std::jthread([this, work = std::move(work), callback = std::move(callback)](std::stop_token stop) {
            const auto started = std::chrono::steady_clock::now();
            // Collect one terminal event, release GPU/media resources first,
            // then permit another job. Progress callbacks must not throw.
            ProcessingEvent terminal;
            const Callback deliver = [&](ProcessingEvent event) {
                if (event.kind == ProcessingEvent::Kind::progress || event.kind == ProcessingEvent::Kind::prepared) callback(std::move(event));
                else terminal = std::move(event);
            };
            try { work(stop, deliver); }
            catch (const std::exception& error) {
                terminal.kind = stop.stop_requested() ? ProcessingEvent::Kind::cancelled : ProcessingEvent::Kind::failed;
                terminal.detail = error.what();
            }
            if (terminal.kind == ProcessingEvent::Kind::progress) {
                terminal.kind = ProcessingEvent::Kind::failed;
                terminal.detail = "Worker completed without a terminal event";
            }
            if (terminal.result) {
                // Include model/device setup and destruction in service-level
                // throughput. The UI must not present inference-only speed.
                auto& result = *terminal.result;
                result.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                result.processingFps = static_cast<double>(result.video.framesSubmitted) / result.elapsedSeconds;
            }
            busy_.store(false);
            callback(std::move(terminal));
        });
    } catch (...) { busy_.store(false); throw; }
    return true;
}
bool ProcessingService::inspect(std::string source, Callback callback) {
    return launch([source = std::move(source)](std::stop_token stop, const Callback& send) {
        ProcessingEvent event;
        event.kind = ProcessingEvent::Kind::inspected;
        event.media = media::MediaEngine::probe(source, stop);
        for (const auto& stream : event.media->streams) if (stream.index == event.media->preferredVideoStream) {
            try { event.sourceInputProfile = media::rifeSdrInputProfile(stream.color); event.sourceColorSupported = true; }
            catch (const std::exception&) {}
            constexpr std::array<const char*,4> encoderNames{"h264_nvenc","libopenh264","libkvazaar","libaom-av1"};
            for (const auto* name : encoderNames) event.encoderCapabilities.push_back(media::MediaEngine::probeEncoder(name,stream,{},stop));
        }
        if (stop.stop_requested()) throw std::runtime_error("Inspection cancelled");
        try { event.device = gpu::CudaDevice().info(); }
        catch (const gpu::GpuError& error) { event.detail = error.what(); }
        send(std::move(event));
    }, std::move(callback));
}
bool ProcessingService::submit(ProcessingJob job, Callback callback) {
    return launch([this, job = std::move(job)](std::stop_token stop, const Callback& send) {
        auto result = executeProcessingJob(job,send,stop,pause_);
        ProcessingEvent event; event.kind = ProcessingEvent::Kind::completed; event.result = std::move(result);
        send(std::move(event));
    }, std::move(callback));
}
} // namespace rifelove::application

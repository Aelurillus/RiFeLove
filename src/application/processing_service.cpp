#include "application/processing_service.hpp"
#include "inference/onnx_cuda_backend.hpp"
#include <chrono>

namespace rifelove::application {
ProcessingService::~ProcessingService() { cancel(); if (worker_.joinable()) worker_.join(); }
void ProcessingService::cancel() { if (worker_.joinable()) worker_.request_stop(); }
bool ProcessingService::launch(std::function<void(std::stop_token, const Callback&)> work, Callback callback) {
    if (!callback) throw std::invalid_argument("A processing callback is required");
    if (busy_.exchange(true)) return false;
    try {
        if (worker_.joinable()) worker_.join();
        worker_ = std::jthread([this, work = std::move(work), callback = std::move(callback)](std::stop_token stop) {
            const auto started = std::chrono::steady_clock::now();
            // Collect one terminal event, release GPU/media resources first,
            // then permit another job. Progress callbacks must not throw.
            ProcessingEvent terminal;
            const Callback deliver = [&](ProcessingEvent event) {
                if (event.kind == ProcessingEvent::Kind::progress) callback(std::move(event));
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
        if (stop.stop_requested()) throw std::runtime_error("Inspection cancelled");
        try { event.device = gpu::CudaDevice().info(); }
        catch (const gpu::GpuError& error) { event.detail = error.what(); }
        send(std::move(event));
    }, std::move(callback));
}
bool ProcessingService::submit(ProcessingJob job, Callback callback) {
    return launch([job = std::move(job)](std::stop_token stop, const Callback& send) {
        const auto started = std::chrono::steady_clock::now();
        ProcessingEvent loading; loading.stage = "loading_model"; send(std::move(loading));
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        const auto pack = inference::loadModelPack(job.modelDirectory);
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        auto device = std::make_shared<gpu::CudaDevice>(job.deviceOrdinal);
        inference::OnnxCudaConfig config; config.runtimeDirectory = job.runtimeDirectory;
        inference::OnnxCudaBackend backend(device, config);
        backend.loadModel(pack);
        auto lastUpdate = std::chrono::steady_clock::now();
        std::string lastStage;
        auto result = interpolateVideo(job.source, job.output, job.settings, *device, backend,
            [&](const PipelineProgress& progress) {
                const auto now = std::chrono::steady_clock::now();
                if (progress.stage == lastStage && now - lastUpdate < std::chrono::milliseconds(100)) return;
                lastUpdate = now; lastStage = progress.stage;
                ProcessingEvent event; event.stage = progress.stage; event.detail = progress.detail;
                event.outputFrames = progress.outputFrames; event.aiFrames = progress.aiFrames;
                event.timestamp = progress.timestamp;
                const double seconds = std::chrono::duration<double>(now - started).count();
                event.processingFps = seconds > 0 ? static_cast<double>(progress.outputFrames) / seconds : 0;
                send(std::move(event));
            }, stop);
        ProcessingEvent event; event.kind = ProcessingEvent::Kind::completed; event.result = std::move(result);
        send(std::move(event));
    }, std::move(callback));
}
} // namespace rifelove::application

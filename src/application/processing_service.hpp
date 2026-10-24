#pragma once
#include "core/pipeline/video_pipeline.hpp"
#include <atomic>
#include <thread>

namespace rifelove::application {
struct ProcessingJob {
    std::string source;
    std::string output;
    std::filesystem::path modelDirectory;
    std::filesystem::path runtimeDirectory;
    PipelineSettings settings;
    int deviceOrdinal = 0;
};
struct ProcessingEvent {
    enum class Kind { progress, inspected, completed, cancelled, failed };
    Kind kind = Kind::progress;
    std::string stage;
    std::string detail;
    std::size_t outputFrames = 0;
    std::size_t aiFrames = 0;
    double processingFps = 0;
    std::optional<Rational> timestamp;
    std::optional<media::MediaInfo> media;
    std::optional<gpu::DeviceInfo> device;
    std::optional<PipelineResult> result;
};
// One bounded worker per service. Call submit/cancel from one owner thread;
// callbacks run on the worker and must marshal owned values to their UI thread.
// Destruction requests cancellation and waits for safe resource cleanup.
class ProcessingService final {
public:
    using Callback = std::function<void(ProcessingEvent)>;
    ~ProcessingService();
    [[nodiscard]] bool inspect(std::string source, Callback callback);
    [[nodiscard]] bool submit(ProcessingJob job, Callback callback);
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return busy_.load(); }
private:
    [[nodiscard]] bool launch(std::function<void(std::stop_token, const Callback&)> work, Callback callback);
    std::jthread worker_;
    std::atomic<bool> busy_{false};
};
} // namespace rifelove::application

#pragma once
#include "application/processing_service.hpp"
#include "database/task_store.hpp"

namespace rifelove::application {
struct QueueEvent {
    std::string taskId;
    ProcessingEvent processing;
    bool drained = false;
    std::string queueError;
};
class TaskQueue final {
public:
    // A single owner calls start/wait/enqueue/retry/restart. Callbacks run on
    // the worker and must not throw or reenter start/wait. Pause/resume/stop/
    // cancel and read-only snapshots may be signalled from other threads.
    using Callback = std::function<void(QueueEvent)>;
    explicit TaskQueue(const std::filesystem::path& database);
    ~TaskQueue();
    [[nodiscard]] std::vector<std::string> enqueue(std::vector<ProcessingJob> jobs, std::stop_token stop = {});
    [[nodiscard]] std::vector<ConfiguredTask> list() const { return store_.configured(); }
    [[nodiscard]] bool start(Callback callback);
    void pause() { pause_.pause(); }
    void resume() { pause_.resume(); }
    void stop();
    void wait(); // Owner thread only; wait for callbacks/resources to finish.
    void cancel(const std::string& id);
    void retry(const std::string& id);
    // Creates a separate task from frame zero, preserving interrupted history.
    [[nodiscard]] std::string restart(const std::string& id, const std::string& newOutput);
    [[nodiscard]] bool busy() const { return busy_.load(); }
    [[nodiscard]] std::size_t recoveredCount() const { return recovered_; }
private:
    void run(std::stop_token stop, const Callback& callback);
    std::unique_ptr<QueueOwner> owner_;
    TaskStore store_;
    std::jthread worker_;
    std::atomic<bool> busy_{false};
    PauseGate pause_;
    std::mutex activity_;
    std::string activeId_;
    std::stop_source currentCancel_;
    std::stop_source runCancel_;
    std::size_t recovered_ = 0;
};
} // namespace rifelove::application

#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <stop_token>

namespace rifelove::application {
class PauseGate final {
public:
    void pause() { std::lock_guard lock(mutex_); requested_ = true; }
    void resume() { { std::lock_guard lock(mutex_); requested_ = false; } condition_.notify_all(); }
    void checkpoint(std::stop_token stop, const std::function<void(bool)>& changed) {
        std::unique_lock lock(mutex_);
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        if (!requested_) return;
        lock.unlock(); changed(true); lock.lock();
        condition_.wait(lock,stop,[&] { return !requested_; });
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled while paused");
        lock.unlock(); changed(false);
    }
private:
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool requested_ = false;
};
} // namespace rifelove::application

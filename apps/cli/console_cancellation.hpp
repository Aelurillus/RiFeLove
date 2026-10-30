#pragma once
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::cli {
inline std::atomic<bool> consoleCancel{false};
inline BOOL WINAPI consoleHandler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    consoleCancel.store(true,std::memory_order_relaxed); return TRUE;
}
class ConsoleCancellation final {
public:
    ConsoleCancellation() {
        consoleCancel.store(false,std::memory_order_relaxed);
        if (!SetConsoleCtrlHandler(consoleHandler,TRUE)) throw std::runtime_error("Could not register console cancellation handler");
        try {
            watcher_ = std::jthread([this](std::stop_token shutdown) {
                while (!shutdown.stop_requested()) {
                    if (consoleCancel.load(std::memory_order_relaxed)) { source_.request_stop(); return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
        } catch (...) { SetConsoleCtrlHandler(consoleHandler,FALSE); throw; }
    }
    ~ConsoleCancellation() { watcher_.request_stop(); if (watcher_.joinable()) watcher_.join(); SetConsoleCtrlHandler(consoleHandler,FALSE); }
    std::stop_token token() const { return source_.get_token(); }
private:
    std::stop_source source_;
    std::jthread watcher_;
};
} // namespace rifelove::cli

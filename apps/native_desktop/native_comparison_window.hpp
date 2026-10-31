#pragma once

#include "native_model_store.hpp"
#include "media/media_engine.hpp"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace rifelove::native_desktop {

struct NativeComparisonImage final {
    int width = 0, height = 0;
    Rational timestamp;
    std::optional<Rational> previous, next;
    std::vector<std::uint8_t> bgra;
    std::optional<Rational> sampleAspectRatio;
};

inline NativeComparisonImage nativeComparisonImage(media::DecodedPreviewFrame frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
        frame.rgba.size() != static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4)
        throw std::runtime_error("Invalid decoded comparison image");
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) std::swap(frame.rgba[i], frame.rgba[i + 2]);
    return {frame.width, frame.height, frame.timestamp, frame.previousTimestamp, frame.nextTimestamp, std::move(frame.rgba), frame.sampleAspectRatio};
}

// Fit the display aspect into the available pane without altering decoded pixels.
inline std::pair<int, int> nativeComparisonFit(int width, int height, int paneWidth, int paneHeight,
    const std::optional<Rational>& sampleAspectRatio) {
    if (width <= 0 || height <= 0 || paneWidth <= 0 || paneHeight <= 0)
        throw std::invalid_argument("Invalid comparison image or pane dimensions");
    double ratio = static_cast<double>(width) / height;
    if (sampleAspectRatio && *sampleAspectRatio > Rational(0))
        ratio *= static_cast<double>(sampleAspectRatio->numerator()) / static_cast<double>(sampleAspectRatio->denominator());
    const double paneRatio = static_cast<double>(paneWidth) / paneHeight;
    if (ratio >= paneRatio)
        return {paneWidth, std::clamp(static_cast<int>(std::lround(paneWidth / ratio)), 1, paneHeight)};
    return {std::clamp(static_cast<int>(std::lround(paneHeight * ratio)), 1, paneWidth), paneHeight};
}

struct NativeComparisonRequest final {
    std::filesystem::path source, output;
    Rational relativeTime;
    std::optional<Rational> previousBefore;
    std::uint64_t generation = 0;
};

struct NativeComparisonPair final {
    NativeComparisonImage source, output;
    Rational sourceOrigin, outputOrigin, requested;
    std::optional<Rational> duration;
};

inline Rational nativeComparisonTime(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n"), last = value.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) throw std::invalid_argument("Comparison time is empty");
    value = value.substr(first, last - first + 1);
    const auto point = value.find(L'.');
    if (point == std::wstring::npos) return Rational::parse(nativeModelUtf8(value));
    const auto fractional = value.size() - point - 1;
    if (fractional > 9 || value.find(L'/', 0) != std::wstring::npos || value.find(L'.', point + 1) != std::wstring::npos)
        throw std::invalid_argument("Comparison seconds support up to nine decimal places or an exact fraction");
    value.erase(point, 1);
    if (value.empty() || value == L"+" || value == L"-") throw std::invalid_argument("Comparison time has no digits");
    std::int64_t denominator = 1;
    for (std::size_t index = 0; index < fractional; ++index) denominator *= 10;
    return Rational::parse(nativeModelUtf8(value)) / Rational(denominator);
}

inline const media::StreamInfo& nativeComparisonStream(const media::MediaInfo& info) {
    const auto stream = std::find_if(info.streams.begin(), info.streams.end(), [&](const auto& item) {
        return item.index == info.preferredVideoStream && item.type == "video";
    });
    if (stream == info.streams.end()) throw std::runtime_error("No comparable video stream is available");
    constexpr std::array<std::int32_t, 9> identity{65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824};
    if ((stream->rotationDegrees && *stream->rotationDegrees != 0.0) ||
        (stream->displayMatrix && *stream->displayMatrix != identity))
        throw std::runtime_error("Rotated, mirrored or offset display-matrix comparison is not yet validated; no transformed substitute is shown");
    return *stream;
}

inline Rational nativeComparisonOrigin(const media::StreamInfo& stream, const std::function<Rational()>& firstTimestamp) {
    return stream.start ? *stream.start : firstTimestamp();
}

inline Rational nativeComparisonAbsoluteTime(Rational origin, Rational relativeTime) {
    try {
        const auto absolute = origin + relativeTime;
        if (absolute.denominator() <= std::numeric_limits<int>::max()) return absolute;
    } catch (const std::overflow_error&) {
        // Refuse an unrepresentable exact request rather than silently seeking
        // to a different relative time in one of the two videos.
    }
    throw std::invalid_argument("该定位时间与视频起始 PTS 的精确组合超出当前比较精度；请减少秒数小数位或使用相邻帧按钮。未显示替代时间。");
}

// Modeless, read-only comparison driven by one relative-time playback clock.
// Playback is muted and capped at 30 preview requests/second; exact stepping
// remains available at the video's full frame rate. A single persistent worker
// coalesces requests; neither seeking nor closing joins a decoder on the UI
// thread. Completions live in shared state rather than raw message pointers.
class NativeComparisonWindow final {
public:
    using Loader = std::function<NativeComparisonPair(const NativeComparisonRequest&, std::stop_token)>;
    explicit NativeComparisonWindow(Loader loader = loadPair)
        : shared_(std::make_shared<Shared>()), background_(CreateSolidBrush(RGB(12, 15, 21))),
          field_(CreateSolidBrush(RGB(20, 25, 34))), worker_([state = shared_, loader = std::move(loader)](std::stop_token stop) {
        for (;;) {
            NativeComparisonRequest request;
            std::stop_source cancellation;
            {
                std::unique_lock lock(state->mutex);
                if (!state->wake.wait(lock, stop, [&] { return state->pending.has_value(); })) return;
                request = std::move(*state->pending);
                state->pending.reset();
                state->active = cancellation;
            }
            std::stop_callback parentCancelled(stop, [cancellation]() mutable { cancellation.request_stop(); });
            Completion result;
            result.generation = request.generation;
            try { result.pair = loader(request, cancellation.get_token()); }
            catch (const std::exception& error) {
                try { result.error = wide(error.what()); }
                catch (...) { result.error = L"无法读取该视频帧"; }
            }
            catch (...) { result.error = L"无法读取该视频帧"; }
            std::lock_guard lock(state->mutex);
            if (stop.stop_requested() || cancellation.stop_requested() || request.generation != state->generation || !state->target) continue;
            state->completed = std::move(result);
            if (!PostMessageW(state->target, loadedMessage, 0, 0)) state->completed.reset();
        }
    }) {}

    ~NativeComparisonWindow() {
        close();
        worker_.request_stop();
        shared_->wake.notify_all();
        if (worker_.joinable()) worker_.join();
        if (font_) DeleteObject(font_);
        if (background_) DeleteObject(background_);
        if (field_) DeleteObject(field_);
    }
    NativeComparisonWindow(const NativeComparisonWindow&) = delete;
    NativeComparisonWindow& operator=(const NativeComparisonWindow&) = delete;

    [[nodiscard]] HWND window() const noexcept { return window_; }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] bool ready() const noexcept { return pair_.has_value() && !busy_ && error_.empty(); }
    [[nodiscard]] const std::wstring& error() const noexcept { return error_; }
    [[nodiscard]] const std::optional<NativeComparisonPair>& pair() const noexcept { return pair_; }

    HWND open(HWND owner, const std::filesystem::path& source = {}, const std::filesystem::path& output = {}) {
        if (!window_) {
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof(cls); cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpfnWndProc = procedure; cls.lpszClassName = L"RifeLoveNativeComparisonWindow";
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.style = CS_HREDRAW | CS_VREDRAW;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
            MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const int dpi = static_cast<int>(owner ? GetDpiForWindow(owner) : GetDpiForSystem());
            const int width = std::min(MulDiv(1180, dpi ? dpi : 96, 96), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
            const int height = std::min(MulDiv(760, dpi ? dpi : 96, 96), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
            window_ = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"RifeLove · 同步比较",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
                width, height, owner, nullptr, cls.hInstance, this);
            if (!window_) return nullptr;
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
            { std::lock_guard lock(shared_->mutex); shared_->target = window_; }
        }
        ShowWindow(window_, SW_SHOWNORMAL);
        if (!source.empty() || !output.empty()) setFiles(source, output);
        UpdateWindow(window_);
        return window_;
    }

    void close() {
        pause();
        invalidateRequests();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr;
        pair_.reset(); busy_ = false;
        source_.clear(); output_.clear(); error_.clear(); pendingTime_ = Rational(0);
        if (font_) { DeleteObject(font_); font_ = nullptr; }
    }

    void setFiles(const std::filesystem::path& source, const std::filesystem::path& output) {
        pause();
        source_ = source; output_ = output; pair_.reset(); error_.clear();
        if (window_) {
            SetWindowTextW(GetDlgItem(window_, sourcePathId), source_.c_str());
            SetWindowTextW(GetDlgItem(window_, outputPathId), output_.c_str());
        }
        if (!source_.empty() && !output_.empty()) seek(Rational(0));
        else {
            invalidateRequests(false); busy_ = false;
            setStatus(L"选择原片和已完成结果。支持同一相对时间的静音同步播放与逐帧比较；未知像素比例时按编码像素显示。");
            updateControls();
        }
    }

    void seek(Rational relativeTime, std::optional<Rational> previousBefore = {}) {
        pause();
        if (window_) KillTimer(window_, seekTimer);
        requestPair(relativeTime, previousBefore);
    }

    void play() {
        if (playing_ || !ready() || !window_) return;
        KillTimer(window_, seekTimer);
        playing_ = true;
        playbackAnchor_ = pair_->requested;
        playbackStarted_ = std::chrono::steady_clock::now();
        const bool atEnd = !pair_->output.next || (pair_->duration ? pair_->requested >= *pair_->duration : !pair_->source.next);
        playbackClockReady_ = !atEnd;
        if (!SetTimer(window_, playbackTimer, 34, nullptr)) {
            playing_ = false;
            setStatus(L"无法启动播放计时器；仍可逐帧查看。");
            updateControls();
            return;
        }
        if (atEnd) requestPair(Rational(0));
        else { showPairStatus(); updateControls(); }
    }

    void pause() {
        if (!playing_) return;
        playing_ = false;
        if (window_) KillTimer(window_, playbackTimer);
        // Freeze the last atomically published pair. A slow in-flight decode
        // must not advance the picture after the user has pressed Pause.
        if (busy_) { invalidateRequests(false); busy_ = false; }
        showPairStatus();
        updateControls();
    }

private:
    void requestPair(Rational relativeTime, std::optional<Rational> previousBefore = {}) {
        if (!window_ || source_.empty() || output_.empty()) return;
        if (relativeTime < Rational(0)) relativeTime = Rational(0);
        if (pair_ && pair_->duration && relativeTime > *pair_->duration) relativeTime = *pair_->duration;
        NativeComparisonRequest request{source_, output_, relativeTime, previousBefore, 0};
        {
            std::lock_guard lock(shared_->mutex);
            shared_->active.request_stop();
            request.generation = ++shared_->generation;
            shared_->pending = std::move(request);
            shared_->completed.reset();
        }
        busy_ = true; error_.clear();
        if (!playing_) setStatus(previousBefore ? L"正在查找前一呈现帧… 可继续定位或关闭。" : L"正在读取同一时间点的原片和处理结果… 可继续定位或关闭。");
        updateControls(); shared_->wake.notify_all();
    }

public:
    void step(int direction) {
        if (direction == 0) return;
        pause();
        if (!ready()) return;
        const auto& output = pair_->output;
        const auto adjacent = direction > 0 ? output.next : output.previous;
        if (adjacent) seek(*adjacent - pair_->outputOrigin);
        else if (direction < 0 && output.timestamp > pair_->outputOrigin)
            seek(output.timestamp - pair_->outputOrigin, output.timestamp);
    }

    bool handleDialogMessage(MSG& message) {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { close(); return true; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && message.hwnd == GetDlgItem(window_, timeId)) {
            SendMessageW(GetDlgItem(window_, goId), BM_CLICK, 0, 0); return true;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_SPACE &&
            message.hwnd != GetDlgItem(window_, timeId) && message.hwnd != GetDlgItem(window_, sourcePathId) && message.hwnd != GetDlgItem(window_, outputPathId) &&
            message.hwnd != GetDlgItem(window_, statusId)) {
            if ((message.lParam & (static_cast<LPARAM>(1) << 30)) != 0) return true;
            if (playing_) pause(); else play();
            return true;
        }
        if (message.message == WM_KEYDOWN && (message.wParam == VK_LEFT || message.wParam == VK_RIGHT) &&
            message.hwnd != GetDlgItem(window_, timeId) && message.hwnd != GetDlgItem(window_, sourcePathId) && message.hwnd != GetDlgItem(window_, outputPathId)) {
            step(message.wParam == VK_LEFT ? -1 : 1); return true;
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }

    [[nodiscard]] static NativeComparisonPair loadPair(const NativeComparisonRequest& request, std::stop_token stop) {
        const auto checkedFile = [](const std::filesystem::path& file) {
            const auto local = nativeModelPath(nativeModelUtf8(file.wstring()));
            (void)checkedNativeModelDirectory(local.parent_path(), false);
            const DWORD attributes = GetFileAttributesW(local.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("Choose an existing local video file (not a link or network path)");
            return nativeModelUtf8(local.wstring());
        };
        const auto source = checkedFile(request.source), output = checkedFile(request.output);
        const auto sourceInfo = media::MediaEngine::probe(source, stop), outputInfo = media::MediaEngine::probe(output, stop);
        const auto& sourceStream = nativeComparisonStream(sourceInfo); const auto& outputStream = nativeComparisonStream(outputInfo);
        const auto origin = [&](const std::string& file, const media::StreamInfo& stream) {
            return nativeComparisonOrigin(stream, [&] {
                struct FoundOrigin { Rational timestamp; };
                try {
                    (void)media::MediaEngine::decodeVideo(file, [&](const media::DecodedFrame& frame) {
                        if (!frame.timestamp || frame.timestampRecovered)
                            throw std::runtime_error("Video start PTS is absent and the first presentation timestamp cannot be verified for same-time comparison");
                        throw FoundOrigin{*frame.timestamp};
                    }, stream.index, stop);
                } catch (const FoundOrigin& first) { return first.timestamp; }
                throw std::runtime_error("No first presentation timestamp is available for same-time comparison");
            });
        };
        NativeComparisonPair result;
        result.sourceOrigin = origin(source, sourceStream);
        result.outputOrigin = origin(output, outputStream);
        result.requested = request.relativeTime;
        const auto sourceDuration = sourceStream.duration ? sourceStream.duration : sourceInfo.duration;
        const auto outputDuration = outputStream.duration ? outputStream.duration : outputInfo.duration;
        if (sourceDuration && *sourceDuration > Rational(0)) result.duration = sourceDuration;
        if (outputDuration && *outputDuration > Rational(0))
            result.duration = result.duration ? std::min(*result.duration, *outputDuration) : outputDuration;
        if (request.previousBefore) {
            // Seeking exactly to a keyframe may omit its predecessor. Decode
            // timestamps from the beginning only in this case, rather than
            // guessing 1/FPS and skipping VFR frames. Cancellation stays live.
            struct FoundPrevious {};
            std::optional<Rational> previous;
            try {
                (void)media::MediaEngine::decodeVideo(output, [&](const media::DecodedFrame& frame) {
                    if (frame.timestamp) {
                        if (*frame.timestamp >= *request.previousBefore) throw FoundPrevious{};
                        previous = frame.timestamp;
                    }
                }, outputStream.index, stop);
            } catch (const FoundPrevious&) {}
            result.requested = previous ? *previous - result.outputOrigin : Rational(0);
        }
        if (result.requested < Rational(0)) result.requested = Rational(0);
        if (result.duration && result.requested > *result.duration) result.requested = *result.duration;
        const auto sourceTime = nativeComparisonAbsoluteTime(result.sourceOrigin, result.requested);
        const auto outputTime = nativeComparisonAbsoluteTime(result.outputOrigin, result.requested);
        result.source = nativeComparisonImage(media::MediaEngine::decodeFrameNearTime(source,
            sourceTime, sourceStream.index, stop));
        result.output = nativeComparisonImage(media::MediaEngine::decodeFrameNearTime(output,
            outputTime, outputStream.index, stop));
        return result;
    }

private:
    static constexpr UINT loadedMessage = WM_APP + 73;
    static constexpr UINT_PTR seekTimer = 73;
    static constexpr UINT_PTR playbackTimer = 74;
    static constexpr int sourcePathId = 1301, outputPathId = 1302, sourceBrowseId = 1303, outputBrowseId = 1304;
    static constexpr int previousId = 1305, nextId = 1306, timeId = 1307, goId = 1308, sliderId = 1309, statusId = 1310, closeId = 1311;
    static constexpr int playId = 1312;
    struct Completion { std::uint64_t generation = 0; std::optional<NativeComparisonPair> pair; std::wstring error; };
    struct Shared {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::optional<NativeComparisonRequest> pending;
        std::optional<Completion> completed;
        std::stop_source active;
        std::uint64_t generation = 0;
        HWND target = nullptr;
    };
    static std::wstring wide(const std::string& value) {
        if (value.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
        if (length) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
        return result;
    }
    static std::wstring seconds(Rational time) {
        std::wostringstream text; text << std::fixed << std::setprecision(3) << time.toLongDouble() << L" s"; return text.str();
    }
    int px(int value) const { const UINT dpi = window_ ? GetDpiForWindow(window_) : 96; return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); }
    void invalidateRequests(bool detach = true) {
        std::lock_guard lock(shared_->mutex);
        ++shared_->generation; shared_->active.request_stop(); shared_->pending.reset(); shared_->completed.reset();
        if (detach) shared_->target = nullptr;
    }
    void setStatus(const std::wstring& text) { if (window_) SetWindowTextW(GetDlgItem(window_, statusId), text.c_str()); }
    void updateControls() {
        if (!window_) return;
        EnableWindow(GetDlgItem(window_, playId), playing_ || ready());
        SetWindowTextW(GetDlgItem(window_, playId), playing_ ? L"暂停" : L"播放 / 静音");
        EnableWindow(GetDlgItem(window_, previousId), ready() && (pair_->output.previous || pair_->output.timestamp > pair_->outputOrigin));
        EnableWindow(GetDlgItem(window_, nextId), ready() && pair_->output.next.has_value());
        EnableWindow(GetDlgItem(window_, sliderId), pair_ && pair_->duration && *pair_->duration > Rational(0));
        EnableWindow(GetDlgItem(window_, goId), !source_.empty() && !output_.empty());
        InvalidateRect(window_, nullptr, FALSE);
    }
    void showPairStatus(bool ended = false) {
        if (!pair_ || !error_.empty()) return;
        const auto position = pair_->output.timestamp - pair_->outputOrigin;
        setStatus((playing_ ? std::wstring(L"同步播放 · 静音 · ") : ended ? std::wstring(L"播放结束 · 静音 · ") : std::wstring(L"已暂停 · 静音 · ")) +
            L"请求 " + seconds(pair_->requested) + L" · 原片 " + seconds(pair_->source.timestamp - pair_->sourceOrigin) +
            L" · 结果 " + seconds(position) + L" · 预览最高 30 FPS，解码较慢时跳过预览帧；逐帧可查看全部帧。未知像素比例时按编码像素显示。");
    }
    void playbackTick() {
        if (!playing_ || !playbackClockReady_ || busy_ || !ready()) return;
        // Do not cancel and restart a slow decode on every tick. Both pictures
        // publish together, then the next request catches up to the same
        // monotonic clock; no independent source/output timers can drift.
        try {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - playbackStarted_).count();
            auto time = playbackAnchor_ + Rational(std::max<std::int64_t>(0, elapsed), 1000);
            if (pair_->duration) time = std::min(time, *pair_->duration);
            if (time > pair_->requested) requestPair(time);
        } catch (const std::exception& error) {
            pause(); error_ = wide(error.what());
            setStatus(L"播放时间轴无法继续：" + error_); updateControls();
        }
    }
    void receive() {
        std::optional<Completion> result;
        { std::lock_guard lock(shared_->mutex); result = std::move(shared_->completed); shared_->completed.reset(); }
        if (!result) return;
        { std::lock_guard lock(shared_->mutex); if (result->generation != shared_->generation) return; }
        busy_ = false; error_ = std::move(result->error); pair_ = std::move(result->pair);
        if (!error_.empty() || !pair_) {
            playing_ = false; KillTimer(window_, playbackTimer);
            pair_.reset(); if (error_.empty()) error_ = L"未返回比较帧";
            setStatus(L"无法显示比较帧：" + error_);
        } else {
            const auto position = pair_->output.timestamp - pair_->outputOrigin;
            const auto value = wide(position.toString());
            SetWindowTextW(GetDlgItem(window_, timeId), value.c_str());
            if (pair_->duration && *pair_->duration > Rational(0))
                SetScrollPos(GetDlgItem(window_, sliderId), SB_CTL, static_cast<int>(std::clamp((position / *pair_->duration).toLongDouble() * 10000.0L, 0.0L, 10000.0L)), TRUE);
            if (playing_ && !playbackClockReady_) {
                playbackAnchor_ = pair_->requested;
                playbackStarted_ = std::chrono::steady_clock::now();
                playbackClockReady_ = true;
            }
            const bool ended = playing_ && (pair_->duration ? pair_->requested >= *pair_->duration :
                !pair_->source.next || !pair_->output.next);
            if (ended) { playing_ = false; KillTimer(window_, playbackTimer); }
            showPairStatus(ended);
        }
        updateControls();
    }
    void choose(bool output) {
        pause();
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrFilter = L"视频文件\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.ts\0所有文件\0*.*\0\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = output ? L"选择已完成的处理结果" : L"选择原始视频";
        if (GetOpenFileNameW(&dialog)) setFiles(output ? source_ : std::filesystem::path(path), output ? std::filesystem::path(path) : output_);
    }
    void layout() {
        if (!window_) return;
        if (!font_) font_ = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        RECT client{}; GetClientRect(window_, &client);
        const int w = client.right, h = client.bottom;
        const bool compact = w < px(760);
        const auto move = [&](int id, int x, int y, int width, int height) {
            const HWND control = GetDlgItem(window_, id);
            SetWindowPos(control, nullptr, x, y, std::max(1, width), std::max(1, height), SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        };
        move(sourcePathId, px(70), px(58), w - px(210), px(32));
        move(outputPathId, px(70), px(98), w - px(210), px(32));
        move(sourceBrowseId, w - px(128), px(56), px(108), px(36));
        move(outputBrowseId, w - px(128), px(96), px(108), px(36));
        move(sliderId, px(20), h - px(compact ? 168 : 126), w - px(40), px(20));
        move(previousId, px(20), h - px(compact ? 136 : 94), px(96), px(34));
        move(nextId, px(124), h - px(compact ? 136 : 94), px(96), px(34));
        move(playId, px(228), h - px(compact ? 136 : 94), px(96), px(34));
        move(timeId, px(compact ? 20 : 340), h - px(92), px(128), px(30));
        move(goId, px(compact ? 156 : 476), h - px(94), px(80), px(34));
        move(closeId, w - px(116), h - px(compact ? 136 : 94), px(96), px(34));
        move(statusId, px(20), h - px(49), w - px(40), px(42));
        InvalidateRect(window_, nullptr, FALSE);
    }
    void draw(HDC dc) {
        const int saved = SaveDC(dc);
        RECT client{}; GetClientRect(window_, &client); FillRect(dc, &client, background_);
        SelectObject(dc, font_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(232, 241, 248));
        const auto text = [&](const std::wstring& value, RECT rect, bool wrap = false) {
            DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, DT_LEFT | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS));
        };
        text(L"同步比较  /  同一相对时间 · 静音播放 · 精确逐帧", RECT{px(20), px(15), client.right - px(20), px(42)});
        text(L"原片", RECT{px(20), px(65), px(66), px(88)});
        text(L"结果", RECT{px(20), px(105), px(66), px(128)});
        const bool compact = client.right < px(760);
        const int gap = px(14), inset = px(20), width = std::max(1, (static_cast<int>(client.right) - inset * 2 - gap) / 2);
        const std::array<const NativeComparisonImage*, 2> images{pair_ ? &pair_->source : nullptr, pair_ ? &pair_->output : nullptr};
        for (int side = 0; side < 2; ++side) {
            RECT pane{inset + side * (width + gap), px(148), inset + side * (width + gap) + width, client.bottom - px(compact ? 184 : 142)};
            if (pane.bottom <= pane.top || pane.right <= pane.left) continue;
            FillRect(dc, &pane, field_);
            RECT title{pane.left + px(12), pane.top + px(10), pane.right - px(12), pane.top + px(32)};
            text(side == 0 ? L"原片" : L"处理结果", title);
            RECT imageArea{pane.left + px(8), pane.top + px(58), pane.right - px(8), pane.bottom - px(8)};
            if (imageArea.right <= imageArea.left || imageArea.bottom <= imageArea.top) continue;
            const auto* image = images[static_cast<std::size_t>(side)];
            if ((!ready() && !(playing_ && pair_ && error_.empty())) || !image) { text(busy_ ? L"正在读取…" : error_.empty() ? L"选择视频以显示画面" : L"当前帧不可用；请查看底部说明", imageArea, true); continue; }
            const auto origin = side == 0 ? pair_->sourceOrigin : pair_->outputOrigin;
            const auto aspect = image->sampleAspectRatio
                ? L"  ·  SAR " + std::to_wstring(image->sampleAspectRatio->numerator()) + L":" + std::to_wstring(image->sampleAspectRatio->denominator())
                : L"  ·  SAR 未知";
            text(std::to_wstring(image->width) + L" × " + std::to_wstring(image->height) + aspect + L"  ·  " + seconds(image->timestamp - origin),
                RECT{title.left, title.bottom, title.right, title.bottom + px(23)});
            const auto [widthPixels, heightPixels] = nativeComparisonFit(image->width, image->height,
                imageArea.right - imageArea.left, imageArea.bottom - imageArea.top, image->sampleAspectRatio);
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = image->width;
            info.bmiHeader.biHeight = -image->height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, imageArea.left + (imageArea.right - imageArea.left - widthPixels) / 2,
                imageArea.top + (imageArea.bottom - imageArea.top - heightPixels) / 2, widthPixels, heightPixels,
                0, 0, image->width, image->height, image->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        }
        RestoreDC(dc, saved);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<NativeComparisonWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<NativeComparisonWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CREATE: {
            const auto control = [&](const wchar_t* kind, const wchar_t* name, DWORD style, int id) {
                return CreateWindowExW(0, kind, name, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            };
            control(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, sourcePathId);
            control(L"EDIT", L"", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, outputPathId);
            control(L"BUTTON", L"选择原片…", WS_TABSTOP | BS_OWNERDRAW, sourceBrowseId);
            control(L"BUTTON", L"选择结果…", WS_TABSTOP | BS_OWNERDRAW, outputBrowseId);
            control(L"BUTTON", L"← 上一帧", WS_TABSTOP | BS_OWNERDRAW, previousId);
            control(L"BUTTON", L"下一帧 →", WS_TABSTOP | BS_OWNERDRAW, nextId);
            control(L"BUTTON", L"播放 / 静音", WS_TABSTOP | BS_OWNERDRAW, playId);
            control(L"EDIT", L"0", WS_TABSTOP | ES_AUTOHSCROLL, timeId);
            control(L"BUTTON", L"定位 / 秒", WS_TABSTOP | BS_OWNERDRAW, goId);
            control(L"BUTTON", L"关闭", WS_TABSTOP | BS_OWNERDRAW, closeId);
            control(L"SCROLLBAR", L"比较位置", WS_TABSTOP | SBS_HORZ, sliderId);
            control(L"EDIT", L"选择原片和已完成结果；支持静音同步播放与精确逐帧。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, statusId);
            SetScrollRange(GetDlgItem(window, sliderId), SB_CTL, 0, 10000, FALSE);
            self->layout(); self->updateControls(); return 0;
        }
        case WM_SIZE: self->layout(); return 0;
        case WM_DPICHANGED: {
            const auto* bounds = reinterpret_cast<const RECT*>(lParam);
            if (self->font_) { DeleteObject(self->font_); self->font_ = nullptr; }
            if (bounds) SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor); GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = std::min<LONG>(self->px(640), monitor.rcWork.right - monitor.rcWork.left);
            info->ptMinTrackSize.y = std::min<LONG>(self->px(480), monitor.rcWork.bottom - monitor.rcWork.top); return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case sourceBrowseId: self->choose(false); return 0;
                case outputBrowseId: self->choose(true); return 0;
                case previousId: self->step(-1); return 0;
                case nextId: self->step(1); return 0;
                case playId: if (self->playing_) self->pause(); else self->play(); return 0;
                case closeId: self->close(); return 0;
                case goId: {
                    wchar_t value[256]{}; GetWindowTextW(GetDlgItem(window, timeId), value, static_cast<int>(std::size(value)));
                    try { self->seek(nativeComparisonTime(value)); }
                    catch (const std::exception&) { self->setStatus(L"请输入有效秒数（最多 9 位小数）或有理数，例如 1.5 或 1/2；可用定位精度还取决于视频起始 PTS，超界时会明确报错。"); }
                    return 0;
                }
                }
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(window, sliderId) && self->pair_ && self->pair_->duration) {
                self->pause();
                const int action = LOWORD(wParam);
                if (action == SB_LINELEFT || action == SB_LINERIGHT) { self->step(action == SB_LINELEFT ? -1 : 1); return 0; }
                int position = GetScrollPos(GetDlgItem(window, sliderId), SB_CTL);
                if (action == SB_THUMBTRACK || action == SB_THUMBPOSITION) position = HIWORD(wParam);
                else if (action == SB_PAGELEFT) position -= 1000;
                else if (action == SB_PAGERIGHT) position += 1000;
                else return 0;
                self->pendingTime_ = *self->pair_->duration * Rational(std::clamp(position, 0, 10000), 10000);
                SetScrollPos(GetDlgItem(window, sliderId), SB_CTL, std::clamp(position, 0, 10000), TRUE);
                KillTimer(window, seekTimer);
                if (action == SB_THUMBTRACK) SetTimer(window, seekTimer, 150, nullptr);
                else self->seek(self->pendingTime_);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == seekTimer) { KillTimer(window, seekTimer); self->seek(self->pendingTime_); return 0; }
            if (wParam == playbackTimer) { self->playbackTick(); return 0; }
            break;
        case loadedMessage: self->receive(); return 0;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam); if (!item) return FALSE;
            const int saved = SaveDC(item->hDC);
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            HBRUSH brush = CreateSolidBrush(disabled ? RGB(29, 38, 49) : (item->itemState & ODS_SELECTED) ? RGB(37, 75, 66) : RGB(33, 52, 66));
            FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, disabled ? RGB(113, 130, 147) : RGB(223, 241, 238)); SelectObject(item->hDC, self->font_);
            wchar_t value[128]{}; GetWindowTextW(item->hwndItem, value, static_cast<int>(std::size(value))); RECT rect = item->rcItem;
            DrawTextW(item->hDC, value, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -self->px(3), -self->px(3)); DrawFocusRect(item->hDC, &rect); }
            RestoreDC(item->hDC, saved); return TRUE;
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(220, 233, 242)); SetBkColor(reinterpret_cast<HDC>(wParam), RGB(20, 25, 34)); return reinterpret_cast<LRESULT>(self->field_);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); self->draw(dc); EndPaint(window, &paint); return 0;
        }
        case WM_PRINTCLIENT: self->draw(reinterpret_cast<HDC>(wParam)); return 0;
        case WM_CLOSE: self->close(); return 0;
        case WM_NCDESTROY:
            self->invalidateRequests(); self->window_ = nullptr; self->pair_.reset(); self->busy_ = false;
            self->source_.clear(); self->output_.clear(); self->error_.clear(); self->pendingTime_ = Rational(0);
            self->playing_ = false;
            KillTimer(window, seekTimer); KillTimer(window, playbackTimer); SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::shared_ptr<Shared> shared_;
    HBRUSH background_ = nullptr, field_ = nullptr;
    HFONT font_ = nullptr;
    HWND window_ = nullptr;
    std::filesystem::path source_, output_;
    std::optional<NativeComparisonPair> pair_;
    std::wstring error_;
    Rational pendingTime_;
    Rational playbackAnchor_;
    std::chrono::steady_clock::time_point playbackStarted_;
    bool playing_ = false, playbackClockReady_ = false;
    bool busy_ = false;
    std::jthread worker_;
};

} // namespace rifelove::native_desktop

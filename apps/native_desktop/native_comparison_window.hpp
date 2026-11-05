#pragma once

#include "native_model_store.hpp"
#include "native_review_resume.hpp"
#include "application/local_lease.hpp"
#include "media/media_engine.hpp"
#include <nlohmann/json.hpp>

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace rifelove::native_desktop {

struct NativeComparisonPixelBuffer final {
    std::shared_ptr<const std::vector<std::uint8_t>> storage;

    NativeComparisonPixelBuffer() = default;
    NativeComparisonPixelBuffer(std::shared_ptr<const std::vector<std::uint8_t>> value) noexcept
        : storage(std::move(value)) {}
    [[nodiscard]] const std::uint8_t* data() const noexcept { return storage ? storage->data() : nullptr; }
    [[nodiscard]] std::size_t size() const noexcept { return storage ? storage->size() : 0; }
    explicit operator bool() const noexcept { return static_cast<bool>(storage); }
    friend bool operator==(const NativeComparisonPixelBuffer& left, const std::vector<std::uint8_t>& right) {
        return left.storage ? *left.storage == right : right.empty();
    }
    friend bool operator==(const std::vector<std::uint8_t>& left, const NativeComparisonPixelBuffer& right) {
        return right == left;
    }
};

struct NativeComparisonImage final {
    int width = 0, height = 0;
    Rational timestamp;
    std::optional<Rational> previous, next;
    NativeComparisonPixelBuffer bgra;
    std::optional<Rational> sampleAspectRatio;
};

inline NativeComparisonImage nativeComparisonImage(media::DecodedPreviewFrame frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
        frame.rgba.size() != static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4)
        throw std::runtime_error("Invalid decoded comparison image");
    for (std::size_t i = 0; i < frame.rgba.size(); i += 4) std::swap(frame.rgba[i], frame.rgba[i + 2]);
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(std::move(frame.rgba));
    return {frame.width, frame.height, frame.timestamp, frame.previousTimestamp, frame.nextTimestamp,
        NativeComparisonPixelBuffer(std::shared_ptr<const std::vector<std::uint8_t>>(std::move(pixels))), frame.sampleAspectRatio};
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

struct NativeComparisonProgress final {
    std::atomic<std::uint64_t> completed{0}, total{0};
};

struct NativeComparisonPlaybackMetrics final {
    std::atomic<std::uint64_t> sourceDecodedFrames{0};
    std::atomic<std::uint64_t> outputDecodedFrames{0};
    std::atomic<std::uint64_t> synchronizedPairs{0};
};

struct NativeComparisonRequest final {
    std::filesystem::path source, output;
    Rational relativeTime;
    std::optional<Rational> previousBefore;
    std::string expectedSourceSha256, expectedOutputSha256;
    std::optional<application::FileIdentity> sourceIdentity, outputIdentity;
    std::optional<Rational> sourceOrigin, outputOrigin;
    std::shared_ptr<NativeComparisonProgress> progress;
    std::uint64_t generation = 0;
};

struct NativeComparisonPair final {
    NativeComparisonImage source, output;
    Rational sourceOrigin, outputOrigin, requested;
    std::optional<Rational> duration;
    int sourceStreamIndex = -1, outputStreamIndex = -1;
    std::optional<application::FileIdentity> sourceIdentity, outputIdentity;
    bool reportHashesMatched = false;
    // Keep report-backed media read-leased for as long as this verified pair
    // remains visible, so a reviewer cannot rate bytes that changed after the
    // hash check but before the next seek or candidate switch.
    std::shared_ptr<application::SourceLease> sourceLease, outputLease;
};

struct NativeQualityReviewPoint final {
    Rational relativeTime;
    std::optional<Rational> endRelativeTime;
    bool transition = false;
    bool psnrPerfect = false;
    double psnrDb = 0;
    double ssim = 0;
};

struct NativeQualityReviewRating final {
    bool recorded = false;
    int naturalness = 0; // 1–5, or -1 when the reviewer cannot judge.
    int artifact = 0; // 0 means no obvious artifact; 1–6 are artifact classes.
    int confidence = 0; // 1–5, or 0 when not recorded.
    std::wstring note;
};

struct NativeQualityReviewCandidate final {
    std::wstring name;
    std::wstring blindLabel;
    std::size_t sourceOrder = 0;
    std::filesystem::path output;
    std::string outputSha256;
    std::optional<application::FileIdentity> verifiedIdentity;
    std::optional<Rational> verifiedOrigin;
    std::vector<NativeQualityReviewPoint> points;
    std::vector<NativeQualityReviewRating> ratings;
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
// Sequential paired decoders align every output PTS against the latest source
// PTS at or before that relative time. Bounded queues apply backpressure while
// the UI consumes frames; neither seeking, pausing nor closing joins a decoder
// on the UI thread.
class NativeComparisonWindow final {
public:
    using Loader = std::function<NativeComparisonPair(const NativeComparisonRequest&, std::stop_token)>;
    explicit NativeComparisonWindow(Loader loader = {}, std::filesystem::path reviewDataRoot = {})
        : reviewDataRoot_(std::move(reviewDataRoot)), shared_(std::make_shared<Shared>(static_cast<bool>(loader))), background_(CreateSolidBrush(RGB(12, 15, 21))),
          field_(CreateSolidBrush(RGB(20, 25, 34))), worker_([state = shared_, loader = loader ? std::move(loader) : Loader(loadPair)](std::stop_token stop) {
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
    }), playbackWorker_([state = shared_](std::stop_token stop) { playbackLoop(state, stop); }) {}

    ~NativeComparisonWindow() {
        close();
        worker_.request_stop();
        shared_->wake.notify_all();
        if (worker_.joinable()) worker_.join();
        playbackWorker_.request_stop();
        shared_->playbackWake.notify_all();
        if (playbackWorker_.joinable()) playbackWorker_.join();
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
        (void)persistQualityReviewResume();
        invalidateRequests();
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr;
        pair_.reset(); busy_ = false;
        playbackMetrics_.reset(); playbackTelemetry_.clear();
        source_.clear(); output_.clear(); error_.clear(); pendingTime_ = Rational(0);
        clearQualityReview();
        if (font_) { DeleteObject(font_); font_ = nullptr; }
    }

    void setFiles(const std::filesystem::path& source, const std::filesystem::path& output) {
        setFilesInternal(source, output, true);
    }

    void seek(Rational relativeTime, std::optional<Rational> previousBefore = {}) {
        seekInternal(relativeTime, previousBefore, false);
    }

    void play() {
        if (integrityChecking_ || playing_ || !ready() || !window_) return;
        reviewCursor_.reset();
        loadCurrentRatingControls();
        KillTimer(window_, seekTimer);
        playing_ = true;
        playbackAnchor_ = pair_->requested;
        const bool atEnd = !pair_->output.next || (pair_->duration ? pair_->requested >= *pair_->duration : !pair_->source.next);
        playbackClockReady_ = false;
        if (!shared_->legacyPlayback) playbackClockReady_ = false;
        else {
            playbackClockReady_ = !atEnd;
            playbackStarted_ = std::chrono::steady_clock::now();
        }
        if (!SetTimer(window_, playbackTimer, shared_->legacyPlayback ? 34 : 8, nullptr)) {
            playing_ = false;
            setStatus(L"无法启动播放计时器；仍可逐帧查看。");
            updateControls();
            return;
        }
        if (atEnd) requestPair(Rational(0));
        else { startPlayback(); showPairStatus(); updateControls(); }
    }

    void pause() {
        const bool wasPlaying = playing_;
        if (wasPlaying) (void)updatePlaybackTelemetry(true);
        playing_ = false;
        if (pair_) pendingTime_ = pair_->requested;
        if (window_) KillTimer(window_, playbackTimer);
        cancelPlayback();
        if (!wasPlaying) return;
        // Freeze the last atomically published pair. A slow in-flight decode
        // must not advance the picture after the user has pressed Pause.
        if (busy_) { invalidateRequests(false); busy_ = false; }
        showPairStatus();
        updateControls();
        if (wasPlaying && !persistQualityReviewResume())
            setStatus(L"本机加密恢复状态保存失败；请导出评分以免关闭后丢失。" + reviewCaption());
    }

private:
    void clearQualityReview() {
        qualityReportPath_.clear(); qualityReference_.clear(); qualityReferenceSha256_.clear();
        qualityReportFingerprint_.clear(); resumeSaveFailed_ = false;
        qualityReviewSessionId_.clear();
        reviewerCode_.clear();
        qualityReferenceIdentity_.reset(); reviewCandidates_.clear(); mediaHashesVerified_ = false; integrityChecking_ = false;
        qualityReferenceOrigin_.reset();
        blindRandomSeedWords_.reset();
        blindReviewAvailable_ = false; blindReviewMode_ = false; blindReviewRevealed_ = false;
        hashProgress_.reset();
        selectedCandidate_ = 0; reviewCursor_.reset();
        if (window_) SendMessageW(GetDlgItem(window_, candidateId), CB_RESETCONTENT, 0, 0);
        loadCurrentRatingControls();
        layout();
        updateControls();
    }

    void setFilesInternal(const std::filesystem::path& source, const std::filesystem::path& output,
        bool clearReview, std::optional<Rational> initialTime = {}) {
        pause();
        if (clearReview) clearQualityReview();
        source_ = source; output_ = output; pair_.reset(); error_.clear();
        playbackMetrics_.reset(); playbackTelemetry_.clear();
        if (window_) {
            SetWindowTextW(GetDlgItem(window_, sourcePathId), source_.c_str());
            const auto displayedOutput = blindReviewMode_ && selectedCandidate_ < reviewCandidates_.size()
                ? reviewCandidates_[selectedCandidate_].blindLabel : output_.wstring();
            SetWindowTextW(GetDlgItem(window_, outputPathId), displayedOutput.c_str());
            SetWindowTextW(GetDlgItem(window_, sourceBrowseId), qualityReference_.empty() ? L"选择原片…" : L"选择参考…");
            SetWindowTextW(GetDlgItem(window_, outputBrowseId), qualityReference_.empty() ? L"选择结果…" : L"选择候选…");
        }
        if (!source_.empty() && !output_.empty()) seekInternal(initialTime.value_or(Rational(0)), {}, true);
        else {
            invalidateRequests(false); busy_ = false;
            setStatus(L"选择原片和已完成结果，或导入本地质量评测报告。支持同步播放与精确逐帧比较。");
            updateControls();
        }
    }

    void seekInternal(Rational relativeTime, std::optional<Rational> previousBefore, bool preserveReview) {
        if (integrityChecking_ && !preserveReview) return;
        pause();
        if (!preserveReview) reviewCursor_.reset();
        if (!preserveReview) loadCurrentRatingControls();
        pendingTime_ = relativeTime;
        if (window_) KillTimer(window_, seekTimer);
        requestPair(relativeTime, previousBefore);
    }

    void requestPair(Rational relativeTime, std::optional<Rational> previousBefore = {}) {
        if (!window_ || source_.empty() || output_.empty()) return;
        if (relativeTime < Rational(0)) relativeTime = Rational(0);
        if (pair_ && pair_->duration && relativeTime > *pair_->duration) relativeTime = *pair_->duration;
        pendingTime_ = relativeTime;
        if (window_) KillTimer(window_, integrityTimer);
        hashProgress_.reset();
        NativeComparisonRequest request;
        request.source = source_; request.output = output_; request.relativeTime = relativeTime;
        request.previousBefore = previousBefore;
        if (!qualityReferenceSha256_.empty() && selectedCandidate_ < reviewCandidates_.size()) {
            const auto& candidate = reviewCandidates_[selectedCandidate_];
            request.expectedSourceSha256 = qualityReferenceSha256_;
            request.expectedOutputSha256 = candidate.outputSha256;
            request.sourceIdentity = qualityReferenceIdentity_;
            request.outputIdentity = candidate.verifiedIdentity;
            request.sourceOrigin = qualityReferenceOrigin_;
            request.outputOrigin = candidate.verifiedOrigin;
            integrityChecking_ = !request.sourceIdentity || !request.outputIdentity;
            if (integrityChecking_) {
                hashProgress_ = std::make_shared<NativeComparisonProgress>();
                request.progress = hashProgress_;
            }
        } else {
            integrityChecking_ = false;
        }
        {
            std::lock_guard lock(shared_->mutex);
            shared_->active.request_stop();
            request.generation = ++shared_->generation;
            shared_->pending = std::move(request);
            shared_->completed.reset();
        }
        busy_ = true; error_.clear();
        if (!playing_) setStatus((integrityChecking_ ? std::wstring(L"正在后台核对评测报告中的媒体 SHA-256；可关闭窗口取消。") :
            previousBefore ? std::wstring(L"正在查找前一呈现帧… 可继续定位或关闭。") :
            std::wstring(L"正在读取同一时间点的左侧和右侧视频… 可继续定位或关闭。")) + reviewCaption());
        updateControls();
        if (integrityChecking_) {
            SetTimer(window_, integrityTimer, 200, nullptr);
            updateIntegrityProgress();
        }
        shared_->wake.notify_all();
    }

    void cancelPlayback() {
        {
            std::lock_guard lock(shared_->mutex);
            shared_->playbackActive.request_stop();
            shared_->playbackPending.reset();
            shared_->playbackFrames.clear();
            shared_->playbackError.clear();
            shared_->playbackDone = true;
            ++shared_->playbackGeneration;
        }
        shared_->playbackWake.notify_all();
    }

    void startPlayback() {
        if (shared_->legacyPlayback || !pair_ || source_.empty() || output_.empty()) return;
        PlaybackRequest request;
        request.source = source_;
        request.output = output_;
        request.sourceStreamIndex = pair_->sourceStreamIndex;
        request.outputStreamIndex = pair_->outputStreamIndex;
        request.seed = *pair_;
        request.metrics = std::make_shared<NativeComparisonPlaybackMetrics>();
        playbackMetrics_ = request.metrics;
        playbackTelemetry_.clear();
        playbackStatsWindow_ = std::chrono::steady_clock::now();
        lastPlaybackDetailsUpdate_ = playbackStatsWindow_;
        lastPlaybackSliderUpdate_ = playbackStatsWindow_;
        lastSourceDecodedSample_ = lastOutputDecodedSample_ = lastSynchronizedSample_ = 0;
        playbackDisplayedPairs_ = playbackSkippedPairs_ = 0;
        {
            std::lock_guard lock(shared_->mutex);
            shared_->playbackActive.request_stop();
            request.generation = ++shared_->playbackGeneration;
            shared_->playbackActive = std::stop_source{};
            shared_->playbackPending = std::move(request);
            shared_->playbackFrames.clear();
            shared_->playbackError.clear();
            shared_->playbackDone = false;
        }
        shared_->playbackWake.notify_all();
    }

public:
    void step(int direction) {
        if (integrityChecking_ || direction == 0) return;
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
        const bool candidateDropdownOpen = SendMessageW(GetDlgItem(window_, candidateId), CB_GETDROPPEDSTATE, 0, 0) != 0 ||
            SendMessageW(GetDlgItem(window_, naturalnessRatingId), CB_GETDROPPEDSTATE, 0, 0) != 0 ||
            SendMessageW(GetDlgItem(window_, artifactRatingId), CB_GETDROPPEDSTATE, 0, 0) != 0 ||
            SendMessageW(GetDlgItem(window_, confidenceRatingId), CB_GETDROPPEDSTATE, 0, 0) != 0;
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { close(); return true; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && message.hwnd == GetDlgItem(window_, timeId)) {
            SendMessageW(GetDlgItem(window_, goId), BM_CLICK, 0, 0); return true;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_SPACE && !integrityChecking_ &&
            message.hwnd != GetDlgItem(window_, timeId) && message.hwnd != GetDlgItem(window_, sourcePathId) && message.hwnd != GetDlgItem(window_, outputPathId) &&
            message.hwnd != GetDlgItem(window_, candidateId) && message.hwnd != GetDlgItem(window_, naturalnessRatingId) &&
            message.hwnd != GetDlgItem(window_, artifactRatingId) && message.hwnd != GetDlgItem(window_, confidenceRatingId) &&
            message.hwnd != GetDlgItem(window_, reviewerNoteId) && message.hwnd != GetDlgItem(window_, recordRatingId) &&
            message.hwnd != GetDlgItem(window_, exportRatingsId) && message.hwnd != GetDlgItem(window_, revealQualityCandidatesId) &&
            message.hwnd != GetDlgItem(window_, reviewerCodeButtonId) &&
            message.hwnd != GetDlgItem(window_, statusId) && !candidateDropdownOpen) {
            if ((message.lParam & (static_cast<LPARAM>(1) << 30)) != 0) return true;
            if (playing_) pause(); else play();
            return true;
        }
        if (message.message == WM_KEYDOWN && (message.wParam == VK_LEFT || message.wParam == VK_RIGHT) && !integrityChecking_ &&
            message.hwnd != GetDlgItem(window_, timeId) && message.hwnd != GetDlgItem(window_, sourcePathId) &&
            message.hwnd != GetDlgItem(window_, outputPathId) && message.hwnd != GetDlgItem(window_, candidateId) &&
            message.hwnd != GetDlgItem(window_, naturalnessRatingId) && message.hwnd != GetDlgItem(window_, artifactRatingId) &&
            message.hwnd != GetDlgItem(window_, confidenceRatingId) && message.hwnd != GetDlgItem(window_, reviewerNoteId) &&
            message.hwnd != GetDlgItem(window_, recordRatingId) && message.hwnd != GetDlgItem(window_, exportRatingsId) &&
            message.hwnd != GetDlgItem(window_, reviewerCodeButtonId) && !candidateDropdownOpen) {
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
        const bool reportBacked = !request.expectedSourceSha256.empty() || !request.expectedOutputSha256.empty();
        if (reportBacked && (request.expectedSourceSha256.empty() || request.expectedOutputSha256.empty()))
            throw std::invalid_argument("Quality report media identities are incomplete");
        NativeComparisonPair result;
        std::shared_ptr<application::SourceLease> sourceLease, outputLease;
        if (reportBacked) {
            sourceLease = std::make_shared<application::SourceLease>(nativeModelPath(source));
            outputLease = std::make_shared<application::SourceLease>(nativeModelPath(output));
            result.sourceLease = sourceLease;
            result.outputLease = outputLease;
            const auto sourceMetadata = sourceLease->metadata();
            const auto outputMetadata = outputLease->metadata();
            const std::uint64_t sourceHashBytes = request.sourceIdentity ? 0 : sourceMetadata.size;
            const std::uint64_t outputHashBytes = request.outputIdentity ? 0 : outputMetadata.size;
            if (outputHashBytes > std::numeric_limits<std::uint64_t>::max() - sourceHashBytes)
                throw std::overflow_error("Quality report media size exceeds verification progress limits");
            const auto progressTotal = std::max<std::uint64_t>(1, sourceHashBytes + outputHashBytes);
            if (request.progress) request.progress->total.store(progressTotal, std::memory_order_relaxed);
            const auto identity = [&](application::SourceLease& lease, const application::FileIdentity& metadata,
                const std::string& expected, const std::optional<application::FileIdentity>& cached,
                std::uint64_t progressOffset, const char* label) {
                if (cached) {
                    auto current = metadata;
                    if (current.size != cached->size || current.modified != cached->modified || current.fileId != cached->fileId ||
                        cached->sha256 != expected)
                        throw std::runtime_error(std::string(label) + " changed since the report media hash was verified; import a fresh report");
                    current.sha256 = cached->sha256;
                    return current;
                }
                auto current = lease.fingerprint(stop, [&](std::uint64_t completed, std::uint64_t) {
                    if (request.progress) request.progress->completed.store(progressOffset + completed, std::memory_order_relaxed);
                });
                if (current.sha256 != expected)
                    throw std::runtime_error(std::string(label) + " SHA-256 does not match the quality report; re-evaluate this video");
                return current;
            };
            result.sourceIdentity = identity(*sourceLease, sourceMetadata, request.expectedSourceSha256,
                request.sourceIdentity, 0, "Reference video");
            result.outputIdentity = identity(*outputLease, outputMetadata, request.expectedOutputSha256,
                request.outputIdentity, sourceHashBytes, "Candidate video");
            if (request.progress) request.progress->completed.store(progressTotal, std::memory_order_relaxed);
            result.reportHashesMatched = true;
        }
        const auto sourceInfo = media::MediaEngine::probe(source, stop), outputInfo = media::MediaEngine::probe(output, stop);
        const auto& sourceStream = nativeComparisonStream(sourceInfo); const auto& outputStream = nativeComparisonStream(outputInfo);
        const auto origin = [&](const std::string& file, const media::StreamInfo& stream,
            const std::optional<Rational>& cached) {
            if (reportBacked && cached) return *cached;
            const auto firstTimestamp = [&] {
                struct FoundOrigin { Rational timestamp; };
                try {
                    (void)media::MediaEngine::decodeVideo(file, [&](const media::DecodedFrame& frame) {
                        if (!frame.timestamp || frame.timestampRecovered)
                            throw std::runtime_error("Video start PTS is absent and the first presentation timestamp cannot be verified for same-time comparison");
                        throw FoundOrigin{*frame.timestamp};
                    }, stream.index, stop);
                } catch (const FoundOrigin& first) { return first.timestamp; }
                throw std::runtime_error("No first presentation timestamp is available for same-time comparison");
            };
            return reportBacked ? firstTimestamp() : nativeComparisonOrigin(stream, firstTimestamp);
        };
        result.sourceOrigin = origin(source, sourceStream, request.sourceOrigin);
        result.outputOrigin = origin(output, outputStream, request.outputOrigin);
        result.sourceStreamIndex = sourceStream.index;
        result.outputStreamIndex = outputStream.index;
        if (reportBacked && result.sourceOrigin != result.outputOrigin)
            throw std::runtime_error("Quality report media do not share the same first presentation timestamp; refresh the report");
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
    static constexpr UINT_PTR integrityTimer = 75;
    static constexpr int sourcePathId = 1301, outputPathId = 1302, sourceBrowseId = 1303, outputBrowseId = 1304;
    static constexpr int previousId = 1305, nextId = 1306, timeId = 1307, goId = 1308, sliderId = 1309, statusId = 1310, closeId = 1311;
    static constexpr int playId = 1312, reportBrowseId = 1313, candidateId = 1314;
    static constexpr int previousReviewId = 1315, nextReviewId = 1316;
    static constexpr int revealQualityCandidatesId = 1317;
    static constexpr int naturalnessRatingId = 1318, artifactRatingId = 1319, recordRatingId = 1320, exportRatingsId = 1321;
    static constexpr int confidenceRatingId = 1322, reviewerNoteId = 1323, reviewerCodeButtonId = 1324;
    struct Completion { std::uint64_t generation = 0; std::optional<NativeComparisonPair> pair; std::wstring error; };
    struct PlaybackRequest {
        std::filesystem::path source, output;
        int sourceStreamIndex = -1, outputStreamIndex = -1;
        NativeComparisonPair seed;
    std::shared_ptr<NativeComparisonPlaybackMetrics> metrics;
        std::uint64_t generation = 0;
    };
    struct PreviewQueue {
        static constexpr std::size_t capacity = 4;
        std::mutex mutex;
        std::condition_variable_any changed;
        std::deque<media::DecodedPreviewFrame> frames;
        std::exception_ptr error;
        bool finished = false;

        bool push(media::DecodedPreviewFrame&& frame, std::stop_token stop) {
            std::unique_lock lock(mutex);
            if (!changed.wait(lock, stop, [&] { return finished || frames.size() < capacity; })) return false;
            if (stop.stop_requested() || finished) return false;
            frames.push_back(std::move(frame));
            lock.unlock();
            changed.notify_all();
            return true;
        }

        std::optional<media::DecodedPreviewFrame> pop(std::stop_token stop) {
            std::unique_lock lock(mutex);
            if (!changed.wait(lock, stop, [&] { return finished || !frames.empty(); }) || stop.stop_requested()) return {};
            if (!frames.empty()) {
                auto frame = std::move(frames.front());
                frames.pop_front();
                lock.unlock();
                changed.notify_all();
                return frame;
            }
            const auto failure = error;
            lock.unlock();
            if (failure) std::rethrow_exception(failure);
            return {};
        }

        void finish(std::exception_ptr failure = {}) {
            {
                std::lock_guard lock(mutex);
                if (!finished) { error = std::move(failure); finished = true; }
            }
            changed.notify_all();
        }
    };
    struct Shared {
        explicit Shared(bool usesInjectedLoader) : legacyPlayback(usesInjectedLoader) {}
        std::mutex mutex;
        std::condition_variable_any wake;
        std::optional<NativeComparisonRequest> pending;
        std::optional<Completion> completed;
        std::stop_source active;
        std::uint64_t generation = 0;
        HWND target = nullptr;
        std::condition_variable_any playbackWake;
        std::optional<PlaybackRequest> playbackPending;
        std::deque<NativeComparisonPair> playbackFrames;
        std::stop_source playbackActive;
        std::uint64_t playbackGeneration = 0;
        std::wstring playbackError;
        bool playbackDone = true;
        bool legacyPlayback = false;
    };

    static bool publishPlaybackFrame(const std::shared_ptr<Shared>& state, std::uint64_t generation,
        NativeComparisonPair&& frame, std::stop_token stop) {
        std::unique_lock lock(state->mutex);
        if (!state->playbackWake.wait(lock, stop, [&] {
                return state->playbackFrames.size() < 3 || generation != state->playbackGeneration;
            }) || stop.stop_requested() || generation != state->playbackGeneration)
            return false;
        state->playbackFrames.push_back(std::move(frame));
        lock.unlock();
        state->playbackWake.notify_all();
        return true;
    }

    static void runPlayback(const std::shared_ptr<Shared>& state, const PlaybackRequest& request, std::stop_token stop) {
        PreviewQueue sourceQueue, outputQueue;
        const auto sourceStart = nativeComparisonAbsoluteTime(request.seed.sourceOrigin,
            request.seed.output.timestamp - request.seed.outputOrigin);
        const auto outputStart = request.seed.output.timestamp;
        const auto produce = [](const std::filesystem::path& path, int streamIndex, Rational start,
            PreviewQueue& queue, std::atomic<std::uint64_t>& decodedFrames, std::stop_token producerStop) {
            try {
                std::optional<Rational> previous;
                const auto summary = media::MediaEngine::decodePreviewVideo(nativeModelUtf8(path.wstring()),
                    [&](media::DecodedPreviewFrame&& frame) {
                        if (frame.timestampRecovered)
                            throw std::runtime_error("Sequential comparison playback requires observed presentation timestamps; a timestamp was reconstructed");
                        if (previous && frame.timestamp <= *previous)
                            throw std::runtime_error("Sequential comparison playback requires strictly increasing presentation timestamps");
                        previous = frame.timestamp;
                        decodedFrames.fetch_add(1, std::memory_order_relaxed);
                        (void)queue.push(std::move(frame), producerStop);
                    }, 1280, 720, streamIndex, producerStop,
                    media::DecodeOptions{media::DecodeAcceleration::preferCuda, 0}, start);
                if (summary.frameCount == 0)
                    throw std::runtime_error("Sequential comparison decoder produced no frames");
                queue.finish();
            } catch (...) {
                queue.finish(std::current_exception());
            }
        };
        std::jthread sourceProducer([&](std::stop_token producerStop) {
            produce(request.source, request.sourceStreamIndex, sourceStart, sourceQueue,
                request.metrics->sourceDecodedFrames, producerStop);
        });
        std::jthread outputProducer([&](std::stop_token producerStop) {
            produce(request.output, request.outputStreamIndex, outputStart, outputQueue,
                request.metrics->outputDecodedFrames, producerStop);
        });

        NativeComparisonImage sourceImage = request.seed.source;
        std::optional<media::DecodedPreviewFrame> nextSource;
        bool sourceFinished = false;
        while (auto output = outputQueue.pop(stop)) {
            if (stop.stop_requested()) break;
            if (output->timestamp <= outputStart) continue;
            const auto relativeTime = output->timestamp - request.seed.outputOrigin;
            if (relativeTime < Rational(0)) continue;
            if (request.seed.duration && relativeTime > *request.seed.duration) break;
            const auto sourceTime = nativeComparisonAbsoluteTime(request.seed.sourceOrigin, relativeTime);
            while (!sourceFinished) {
                if (!nextSource) {
                    nextSource = sourceQueue.pop(stop);
                    if (!nextSource) { sourceFinished = true; break; }
                }
                if (nextSource->timestamp < request.seed.sourceOrigin) {
                    nextSource.reset();
                    continue;
                }
                if (nextSource->timestamp > sourceTime) break;
                sourceImage = nativeComparisonImage(std::move(*nextSource));
                nextSource.reset();
            }

            NativeComparisonPair frame;
            frame.source = sourceImage;
            frame.output = nativeComparisonImage(std::move(*output));
            frame.sourceOrigin = request.seed.sourceOrigin;
            frame.outputOrigin = request.seed.outputOrigin;
            frame.requested = relativeTime;
            frame.duration = request.seed.duration;
            frame.sourceStreamIndex = request.seed.sourceStreamIndex;
            frame.outputStreamIndex = request.seed.outputStreamIndex;
            frame.sourceIdentity = request.seed.sourceIdentity;
            frame.outputIdentity = request.seed.outputIdentity;
            frame.reportHashesMatched = request.seed.reportHashesMatched;
            frame.sourceLease = request.seed.sourceLease;
            frame.outputLease = request.seed.outputLease;
            if (!publishPlaybackFrame(state, request.generation, std::move(frame), stop)) break;
            request.metrics->synchronizedPairs.fetch_add(1, std::memory_order_relaxed);
        }
        sourceProducer.request_stop();
        outputProducer.request_stop();
        sourceQueue.changed.notify_all();
        outputQueue.changed.notify_all();
        if (sourceProducer.joinable()) sourceProducer.join();
        if (outputProducer.joinable()) outputProducer.join();
    }

    static void playbackLoop(const std::shared_ptr<Shared>& state, std::stop_token stop) {
        for (;;) {
            PlaybackRequest request;
            std::stop_source cancellation;
            {
                std::unique_lock lock(state->mutex);
                if (!state->playbackWake.wait(lock, stop, [&] { return state->playbackPending.has_value(); })) return;
                request = std::move(*state->playbackPending);
                state->playbackPending.reset();
                cancellation = state->playbackActive;
            }
            std::stop_callback parentCancelled(stop, [cancellation]() mutable { cancellation.request_stop(); });
            std::wstring error;
            try { runPlayback(state, request, cancellation.get_token()); }
            catch (const std::exception& failure) {
                try { error = wide(failure.what()); }
                catch (...) { error = L"同步预览解码失败"; }
            }
            catch (...) { error = L"同步预览解码失败"; }
            {
                std::lock_guard lock(state->mutex);
                if (stop.stop_requested() || cancellation.stop_requested() || request.generation != state->playbackGeneration)
                    continue;
                state->playbackError = std::move(error);
                state->playbackDone = true;
            }
            state->playbackWake.notify_all();
        }
    }

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
    static std::string newQualityReviewSessionId() {
        std::random_device entropy;
        std::ostringstream value;
        value << std::hex << std::setfill('0');
        for (int index = 0; index < 4; ++index)
            value << std::setw(8) << static_cast<std::uint32_t>(entropy());
        return value.str();
    }
    static bool validReviewerCode(const std::wstring& value) {
        if (value.empty()) return true;
        if (value.size() > 32) return false;
        const auto alphanumeric = [](wchar_t character) {
            return (character >= L'A' && character <= L'Z') || (character >= L'a' && character <= L'z') ||
                (character >= L'0' && character <= L'9');
        };
        if (!alphanumeric(value.front())) return false;
        return std::all_of(value.begin() + 1, value.end(), [&](wchar_t character) {
            return alphanumeric(character) || character == L'_' || character == L'-';
        });
    }
    struct ReviewerCodeDialogState { std::wstring value; };
    static std::vector<WORD> reviewerCodeDialogTemplate() {
        static_assert(sizeof(wchar_t) == sizeof(WORD));
        std::vector<WORD> words(sizeof(DLGTEMPLATE) / sizeof(WORD));
        auto* dialog = reinterpret_cast<DLGTEMPLATE*>(words.data());
        dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
        dialog->dwExtendedStyle = 0;
        dialog->cdit = 0;
        dialog->x = 10; dialog->y = 10; dialog->cx = 260; dialog->cy = 92;
        words.push_back(0); // no menu
        words.push_back(0); // default dialog class
        const auto appendText = [&](const wchar_t* text) {
            while (*text) words.push_back(static_cast<WORD>(*text++));
            words.push_back(0);
        };
        appendText(L"设置匿名评审代号");
        words.push_back(9);
        appendText(L"Microsoft YaHei UI");
        return words;
    }
    static INT_PTR CALLBACK reviewerCodeDialogProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* state = reinterpret_cast<ReviewerCodeDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            state = reinterpret_cast<ReviewerCodeDialogState*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            const auto controlRect = [&](int x, int y, int width, int height) {
                RECT rect{x, y, x + width, y + height};
                MapDialogRect(dialog, &rect);
                return rect;
            };
            const auto create = [&](DWORD extended, const wchar_t* type, const wchar_t* title, DWORD style,
                int id, int x, int y, int width, int height) {
                const RECT rect = controlRect(x, y, width, height);
                return CreateWindowExW(extended, type, title, WS_CHILD | WS_VISIBLE | style,
                    rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, dialog,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            };
            create(0, L"STATIC", L"团队匿名代码，最多 32 位；不要填写姓名、邮箱或账号。\n同一评审者跨场次使用同码；首条评分后不可修改。", SS_LEFT,
                2001, 10, 7, 240, 24);
            const HWND edit = create(WS_EX_CLIENTEDGE, L"EDIT", state ? state->value.c_str() : L"",
                WS_TABSTOP | ES_AUTOHSCROLL, 2002, 10, 31, 240, 16);
            if (edit) { SendMessageW(edit, EM_LIMITTEXT, 32, 0); SendMessageW(edit, EM_SETSEL, 0, -1); }
            create(0, L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 156, 66, 44, 16);
            create(0, L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 204, 66, 46, 16);
            if (!edit || !GetDlgItem(dialog, IDOK) || !GetDlgItem(dialog, IDCANCEL)) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            SetFocus(edit);
            return FALSE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
            const int length = GetWindowTextLengthW(GetDlgItem(dialog, 2002));
            std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
            const int copied = GetWindowTextW(GetDlgItem(dialog, 2002), value.data(), length + 1);
            value.resize(static_cast<std::size_t>(std::max(0, copied)));
            if (!validReviewerCode(value)) {
                MessageBoxW(dialog, L"匿名代号限 1–32 位；首位为字母或数字，其余可使用字母、数字、下划线或连字符。", L"评审代号格式无效", MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dialog, 2002));
                return TRUE;
            }
            if (state) state->value = std::move(value);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (message == WM_CLOSE) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    static std::filesystem::path reportLocalPath(const nlohmann::json& value) {
        if (!value.is_string()) throw std::invalid_argument("质量报告中的视频路径不是字符串");
        return nativeModelPath(value.get<std::string>());
    }
    static std::filesystem::path reportVideoPath(const nlohmann::json& value) {
        const auto path = reportLocalPath(value);
        (void)checkedNativeModelDirectory(path.parent_path(), false);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::invalid_argument("质量报告引用的视频不存在，或使用了链接文件");
        return path;
    }
    static std::string reportSha256(const nlohmann::json& file) {
        if (!file.is_object() || !file.contains("sha256") || !file.at("sha256").is_string())
            throw std::invalid_argument("质量报告缺少媒体 SHA-256");
        const auto digest = file.at("sha256").get<std::string>();
        if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) throw std::invalid_argument("质量报告中的媒体 SHA-256 格式无效");
        return digest;
    }
    static double reportScore(const nlohmann::json& value, const char* name) {
        if (!value.contains(name) || !value.at(name).is_number())
            throw std::invalid_argument(std::string("质量报告缺少有效指标：") + name);
        const double score = value.at(name).get<double>();
        if (!std::isfinite(score)) throw std::invalid_argument(std::string("质量报告指标不是有限数值：") + name);
        return score;
    }
    static double reportCandidateScore(const nlohmann::json& value, const char* name, bool& perfect) {
        if (value.is_object() && value.contains(name) && value.at(name).is_string() &&
            value.at(name).get<std::string>() == "perfect") {
            perfect = true;
            return 0;
        }
        return reportScore(value, name);
    }
    static NativeQualityReviewPoint reportPoint(const nlohmann::json& value, bool transition,
        const std::string* candidateName = nullptr) {
        if (!value.is_object()) throw std::invalid_argument("质量报告复核项格式无效");
        const nlohmann::json* scores = &value;
        if (candidateName) {
            if (!value.contains("candidateScores") || !value.at("candidateScores").is_object() ||
                !value.at("candidateScores").contains(*candidateName))
                throw std::invalid_argument("共享复核点缺少候选指标");
            scores = &value.at("candidateScores").at(*candidateName);
        }
        const auto readTime = [&](const char* name) {
            if (!value.contains(name) || !value.at(name).is_string())
                throw std::invalid_argument(std::string("质量报告缺少复核时间：") + name);
            return nativeComparisonTime(wide(value.at(name).get<std::string>()));
        };
        NativeQualityReviewPoint point;
        point.transition = transition;
        point.relativeTime = readTime(transition ? "startRelativeTime" : "relativeTime");
        if (point.relativeTime < Rational(0)) throw std::invalid_argument("质量报告复核时间不能早于视频起点");
        if (transition) {
            point.endRelativeTime = readTime("endRelativeTime");
            if (*point.endRelativeTime <= point.relativeTime)
                throw std::invalid_argument("质量报告过渡时间范围无效");
            point.psnrDb = candidateName ? reportCandidateScore(*scores, "temporalDifferencePsnrDb", point.psnrPerfect) :
                reportScore(*scores, "temporalDifferencePsnrDb");
            point.ssim = reportScore(*scores, "temporalDifferenceSsim");
        } else {
            point.psnrDb = candidateName ? reportCandidateScore(*scores, "psnrDb", point.psnrPerfect) :
                reportScore(*scores, "psnrDb");
            point.ssim = reportScore(*scores, "ssim");
        }
        return point;
    }
    static std::vector<NativeQualityReviewPoint> reportPoints(const nlohmann::json& review,
        const std::string* candidateName = nullptr) {
        if (!review.is_object() || !review.contains("frameScoreRankedByLowestPsnrDb") ||
            !review.contains("transitionScoreRankedByLowestPsnrDb") ||
            !review.at("frameScoreRankedByLowestPsnrDb").is_array() ||
            !review.at("transitionScoreRankedByLowestPsnrDb").is_array())
            throw std::invalid_argument("质量报告缺少帧或过渡复核清单");
        const auto& frames = review.at("frameScoreRankedByLowestPsnrDb");
        const auto& transitions = review.at("transitionScoreRankedByLowestPsnrDb");
        if (frames.size() > 1000 || transitions.size() > 1000)
            throw std::invalid_argument("质量报告复核清单超过 1000 项限制");
        std::vector<NativeQualityReviewPoint> points;
        points.reserve(frames.size() + transitions.size());
        for (const auto& frame : frames) points.push_back(reportPoint(frame, false, candidateName));
        for (const auto& transition : transitions) points.push_back(reportPoint(transition, true, candidateName));
        std::stable_sort(points.begin(), points.end(), [](const auto& left, const auto& right) {
            return left.relativeTime < right.relativeTime;
        });
        return points;
    }
    void chooseQualityReport() {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrFilter = L"RifeLove 质量报告 (*.json)\0*.json\0所有文件\0*.*\0\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = L"导入本地质量评测报告";
        if (!GetOpenFileNameW(&dialog)) return;
        try { loadQualityReport(std::filesystem::path(path)); }
        catch (const std::exception& error) { setStatus(L"导入质量报告失败：" + wide(error.what())); }
    }
    struct RestoredQualityReviewSession final {
        std::string sessionId;
        std::array<std::uint32_t, 4> seedWords{};
        std::vector<std::size_t> candidateOrder;
        std::vector<std::vector<NativeQualityReviewRating>> ratingsBySource;
        std::size_t selectedSourceCandidate = 0;
        std::optional<std::size_t> selectedReviewPoint;
        Rational position;
        std::wstring reviewerCode;
        bool blindReviewMode = true;
    };
    static RestoredQualityReviewSession parseQualityReviewResume(const nlohmann::json& state,
        const std::vector<NativeQualityReviewCandidate>& candidates, const std::string& reportFingerprint) {
        const auto requireIndex = [](const nlohmann::json& value, std::size_t maximum, const char* label) {
            if (!value.is_number_integer()) throw std::invalid_argument(std::string("Invalid saved ") + label);
            const auto index = value.get<std::int64_t>();
            if (index < 0 || static_cast<std::uint64_t>(index) > maximum)
                throw std::invalid_argument(std::string("Invalid saved ") + label);
            return static_cast<std::size_t>(index);
        };
        if (!state.is_object() || state.value("schemaVersion", 0) != 1 ||
            state.value("reportFingerprint", std::string()) != reportFingerprint ||
            !state.contains("sessionId") || !state.at("sessionId").is_string() ||
            !state.contains("seedWords") || !state.at("seedWords").is_array() || state.at("seedWords").size() != 4 ||
            !state.contains("candidateOrder") || !state.at("candidateOrder").is_array() ||
            state.at("candidateOrder").size() != candidates.size() || candidates.size() < 2 || candidates.size() > 64 ||
            !state.contains("candidateSha256") || !state.at("candidateSha256").is_array() ||
            state.at("candidateSha256").size() != candidates.size() ||
            !state.contains("ratings") || !state.at("ratings").is_array() || state.at("ratings").size() != candidates.size() ||
            !state.contains("selectedSourceCandidate") || !state.contains("selectedReviewPoint") ||
            !state.contains("position") || !state.at("position").is_string() ||
            !state.contains("reviewerCode") || !state.at("reviewerCode").is_string() ||
            !state.contains("blindReviewMode") || !state.at("blindReviewMode").is_boolean())
            throw std::invalid_argument("Saved review-session schema or report identity does not match");

        RestoredQualityReviewSession restored;
        restored.sessionId = state.at("sessionId").get<std::string>();
        if (restored.sessionId.size() != 32 || !std::all_of(restored.sessionId.begin(), restored.sessionId.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) throw std::invalid_argument("Saved review-session identifier is invalid");
        for (std::size_t index = 0; index < restored.seedWords.size(); ++index) {
            const auto& word = state.at("seedWords").at(index);
            if (!word.is_number_integer()) throw std::invalid_argument("Saved blind-review seed is invalid");
            const auto value = word.get<std::int64_t>();
            if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Saved blind-review seed is invalid");
            restored.seedWords[index] = static_cast<std::uint32_t>(value);
        }
        std::vector<bool> seen(candidates.size(), false);
        for (const auto& sourceIndexValue : state.at("candidateOrder")) {
            const auto sourceIndex = requireIndex(sourceIndexValue, candidates.size() - 1, "candidate order");
            if (seen[sourceIndex]) throw std::invalid_argument("Saved blind-review order contains duplicates");
            seen[sourceIndex] = true;
            restored.candidateOrder.push_back(sourceIndex);
        }
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& savedHash = state.at("candidateSha256").at(index);
            if (!savedHash.is_string() || savedHash.get<std::string>() != candidates[index].outputSha256)
                throw std::invalid_argument("Saved review session references different candidate outputs");
        }
        restored.ratingsBySource.resize(candidates.size());
        for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            const auto& savedRatings = state.at("ratings").at(candidateIndex);
            if (!savedRatings.is_array() || savedRatings.size() != candidates[candidateIndex].points.size())
                throw std::invalid_argument("Saved review-session point count does not match the report");
            auto& ratings = restored.ratingsBySource[candidateIndex];
            ratings.resize(savedRatings.size());
            for (std::size_t pointIndex = 0; pointIndex < savedRatings.size(); ++pointIndex) {
                const auto& savedRating = savedRatings.at(pointIndex);
                if (savedRating.is_null()) continue;
                if (!savedRating.is_object() || !savedRating.value("recorded", false) ||
                    !savedRating.contains("naturalness") || !savedRating.contains("artifact") ||
                    !savedRating.contains("confidence"))
                    throw std::invalid_argument("Saved review rating is invalid");
                const int naturalness = savedRating.at("naturalness").get<int>();
                const int artifact = savedRating.at("artifact").get<int>();
                const int confidence = savedRating.at("confidence").get<int>();
                if ((naturalness != -1 && (naturalness < 1 || naturalness > 5)) || artifact < 0 || artifact > 6 ||
                    confidence < 0 || confidence > 5)
                    throw std::invalid_argument("Saved review rating value is out of range");
                auto& rating = ratings[pointIndex];
                rating.recorded = true;
                rating.naturalness = naturalness;
                rating.artifact = artifact;
                rating.confidence = confidence;
                if (savedRating.contains("note")) {
                    if (!savedRating.at("note").is_string()) throw std::invalid_argument("Saved review note is invalid");
                    const auto note = savedRating.at("note").get<std::string>();
                    if (note.size() > 64 * 1024) throw std::invalid_argument("Saved review note exceeds its size limit");
                    rating.note = wide(note);
                }
            }
        }
        restored.selectedSourceCandidate = requireIndex(state.at("selectedSourceCandidate"), candidates.size() - 1,
            "selected candidate");
        if (std::find(restored.candidateOrder.begin(), restored.candidateOrder.end(), restored.selectedSourceCandidate) ==
            restored.candidateOrder.end()) throw std::invalid_argument("Saved selected candidate is missing from its order");
        if (!state.at("selectedReviewPoint").is_null()) {
            const auto selectedPoint = requireIndex(state.at("selectedReviewPoint"),
                candidates[restored.selectedSourceCandidate].points.empty() ? 0 :
                    candidates[restored.selectedSourceCandidate].points.size() - 1, "selected review point");
            if (candidates[restored.selectedSourceCandidate].points.empty())
                throw std::invalid_argument("Saved review point exists for an empty candidate");
            restored.selectedReviewPoint = selectedPoint;
        }
        restored.position = Rational::parse(state.at("position").get<std::string>());
        if (restored.position < Rational(0)) throw std::invalid_argument("Saved comparison position is negative");
        restored.reviewerCode = wide(state.at("reviewerCode").get<std::string>());
        if (!validReviewerCode(restored.reviewerCode)) throw std::invalid_argument("Saved reviewer code is invalid");
        restored.blindReviewMode = state.at("blindReviewMode").get<bool>();
        return restored;
    }
    nlohmann::json qualityReviewResumeDocument() const {
        nlohmann::json state;
        state["schemaVersion"] = 1;
        state["reportFingerprint"] = qualityReportFingerprint_;
        state["sessionId"] = qualityReviewSessionId_;
        state["blindReviewMode"] = blindReviewMode_;
        state["reviewerCode"] = nativeModelUtf8(reviewerCode_);
        state["seedWords"] = nlohmann::json::array();
        if (blindRandomSeedWords_) {
            for (const auto word : *blindRandomSeedWords_) state["seedWords"].push_back(word);
        } else {
            for (int index = 0; index < 4; ++index) state["seedWords"].push_back(0);
        }
        state["candidateOrder"] = nlohmann::json::array();
        state["candidateSha256"] = nlohmann::json::array();
        state["ratings"] = nlohmann::json::array();
        std::vector<const NativeQualityReviewCandidate*> candidatesBySource(reviewCandidates_.size(), nullptr);
        for (const auto& candidate : reviewCandidates_) {
            if (candidate.sourceOrder >= candidatesBySource.size() || candidatesBySource[candidate.sourceOrder])
                throw std::runtime_error("Review candidate source order is invalid");
            candidatesBySource[candidate.sourceOrder] = &candidate;
            state["candidateOrder"].push_back(candidate.sourceOrder);
        }
        for (const auto* candidate : candidatesBySource) {
            if (!candidate) throw std::runtime_error("Review candidate source order is incomplete");
            state["candidateSha256"].push_back(candidate->outputSha256);
            nlohmann::json ratings = nlohmann::json::array();
            for (const auto& rating : candidate->ratings) {
                if (!rating.recorded) { ratings.push_back(nullptr); continue; }
                nlohmann::json saved{{"recorded", true}, {"naturalness", rating.naturalness},
                    {"artifact", rating.artifact}, {"confidence", rating.confidence}};
                if (!rating.note.empty()) saved["note"] = nativeModelUtf8(rating.note);
                ratings.push_back(std::move(saved));
            }
            state["ratings"].push_back(std::move(ratings));
        }
        if (selectedCandidate_ < reviewCandidates_.size()) {
            state["selectedSourceCandidate"] = reviewCandidates_[selectedCandidate_].sourceOrder;
            if (reviewCursor_ && *reviewCursor_ < reviewCandidates_[selectedCandidate_].points.size())
                state["selectedReviewPoint"] = *reviewCursor_;
            else state["selectedReviewPoint"] = nullptr;
        } else {
            state["selectedSourceCandidate"] = 0;
            state["selectedReviewPoint"] = nullptr;
        }
        const Rational position = busy_ ? pendingTime_ : pair_ ? pair_->requested : pendingTime_;
        state["position"] = position.toString();
        return state;
    }
    bool persistQualityReviewResume() noexcept {
        if (!blindReviewAvailable_ || qualityReportFingerprint_.empty()) return true;
        try {
            if (reviewDataRoot_.empty()) throw std::runtime_error("Encrypted review-session storage is unavailable");
            saveNativeReviewResume(reviewDataRoot_, qualityReportFingerprint_, qualityReviewResumeDocument());
            resumeSaveFailed_ = false;
            return true;
        } catch (...) {
            resumeSaveFailed_ = true;
            return false;
        }
    }
    void loadQualityReport(const std::filesystem::path& requested) {
        const auto path = std::filesystem::absolute(requested).lexically_normal();
        (void)checkedNativeModelDirectory(path.parent_path(), false);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            throw std::invalid_argument("质量报告必须是本地普通文件");
        const auto size = std::filesystem::file_size(path);
        if (size == 0 || size > 16 * 1024 * 1024) throw std::invalid_argument("质量报告为空或超过 16 MiB");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取质量报告");
        std::string contents(static_cast<std::size_t>(size), '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size()) || input.bad())
            throw std::runtime_error("读取质量报告时发生错误");
        const auto reportFingerprint = nativeReviewSha256(contents);
        const auto report = nlohmann::json::parse(contents);
        if (!report.is_object() || !report.contains("schemaVersion") || !report.at("schemaVersion").is_number_integer())
            throw std::invalid_argument("不支持的质量报告格式");
        const int schema = report.at("schemaVersion").get<int>();
        if ((schema != 1 && schema != 2) || report.value("status", std::string()) != "completed" ||
            !report.value("inputHashesUnchanged", false))
            throw std::invalid_argument("只支持已完成且输入未变化的 schema v1/v2 质量报告");
        if (schema == 1 && (!report.contains("timeline") || !report.at("timeline").is_object() ||
            !report.at("timeline").value("referenceAndResultPtsIdentical", false)))
            throw std::invalid_argument("schema v1 报告未确认参考与结果的逐帧 PTS 一致");
        if (schema == 2 && (!report.contains("comparison") || !report.at("comparison").is_object() ||
            !report.at("comparison").value("allCandidatePtsMatchReference", false)))
            throw std::invalid_argument("schema v2 报告未确认候选与参考的逐帧 PTS 一致");
        const nlohmann::json* sharedReviewSamples = nullptr;
        if (schema == 2 && report.at("comparison").contains("sharedReviewSamples")) {
            if (!report.at("comparison").at("sharedReviewSamples").is_object())
                throw std::invalid_argument("schema v2 共享复核清单格式无效");
            sharedReviewSamples = &report.at("comparison").at("sharedReviewSamples");
        }
        if (!report.contains("files") || !report.at("files").is_object() ||
            !report.at("files").contains("source") || !report.at("files").contains("reference"))
            throw std::invalid_argument("质量报告缺少原片或参考视频");
        const auto source = reportLocalPath(report.at("files").at("source").at("path"));
        const auto reference = reportVideoPath(report.at("files").at("reference").at("path"));
        const auto referenceSha256 = reportSha256(report.at("files").at("reference"));
        if (NativeModelPreferences::key(source) == NativeModelPreferences::key(reference))
            throw std::invalid_argument("质量报告中的原片和参考不能是同一文件");

        std::vector<NativeQualityReviewCandidate> candidates;
        std::optional<std::array<std::uint32_t, 4>> randomizationSeedWords;
        if (schema == 1) {
            if (!report.at("files").contains("result") || !report.contains("reviewSamples"))
                throw std::invalid_argument("schema v1 报告缺少结果或复核清单");
            NativeQualityReviewCandidate candidate;
            candidate.name = L"单结果";
            candidate.output = reportVideoPath(report.at("files").at("result").at("path"));
            candidate.outputSha256 = reportSha256(report.at("files").at("result"));
            candidate.points = reportPoints(report.at("reviewSamples"));
            candidate.ratings.resize(candidate.points.size());
            candidates.push_back(std::move(candidate));
        } else {
            if (!report.contains("candidates") || !report.at("candidates").is_array() ||
                report.at("candidates").empty() || report.at("candidates").size() > 64)
                throw std::invalid_argument("schema v2 候选数量无效（支持 1–64 个）");
            std::size_t totalPoints = 0;
            for (const auto& item : report.at("candidates")) {
                if (!item.is_object() || !item.contains("name") || !item.at("name").is_string() ||
                    !item.contains("output") || !item.contains("reviewSamples"))
                    throw std::invalid_argument("schema v2 候选字段不完整");
                const auto name = item.at("name").get<std::string>();
                if (name.empty() || name.size() > 512) throw std::invalid_argument("候选名称为空或过长");
                NativeQualityReviewCandidate candidate;
                candidate.sourceOrder = candidates.size();
                candidate.name = wide(name);
                candidate.output = reportVideoPath(item.at("output").at("path"));
                candidate.outputSha256 = reportSha256(item.at("output"));
                candidate.points = sharedReviewSamples ? reportPoints(*sharedReviewSamples, &name) :
                    reportPoints(item.at("reviewSamples"));
                candidate.ratings.resize(candidate.points.size());
                totalPoints += candidate.points.size();
                if (totalPoints > 10000) throw std::invalid_argument("质量报告复核项总数超过 10000 项限制");
                candidates.push_back(std::move(candidate));
            }
            const auto declaredCount = report.at("comparison").value("candidateCount", std::size_t{});
            if (declaredCount != candidates.size()) throw std::invalid_argument("质量报告声明的候选数量与内容不一致");
        }
        std::vector<std::wstring> candidateKeys;
        const auto sourceKey = NativeModelPreferences::key(source);
        const auto referenceKey = NativeModelPreferences::key(reference);
        for (const auto& candidate : candidates) {
            const auto key = NativeModelPreferences::key(candidate.output);
            if (key == referenceKey || key == sourceKey || std::find(candidateKeys.begin(), candidateKeys.end(), key) != candidateKeys.end())
                throw std::invalid_argument("候选结果路径重复，或与原片/参考视频相同");
            candidateKeys.push_back(key);
        }

        const bool blindReview = schema == 2 && candidates.size() > 1 && sharedReviewSamples;
        std::optional<RestoredQualityReviewSession> restoredSession;
        std::wstring resumeNotice;
        if (blindReview && !reviewDataRoot_.empty()) {
            try {
                if (nativeReviewResumeExists(reviewDataRoot_, reportFingerprint)) {
                    const auto saved = loadNativeReviewResume(reviewDataRoot_, reportFingerprint);
                    if (saved) {
                        auto parsed = parseQualityReviewResume(*saved, candidates, reportFingerprint);
                        const int choice = MessageBoxW(window_,
                            L"找到此质量报告对应的本机加密盲评场次。\n\n是：恢复评分、候选顺序和复核位置。\n否：开始新的随机盲评；未导出的旧评分会被新场次替换。\n取消：保留当前窗口状态。",
                            L"恢复盲评场次", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
                        if (choice == IDCANCEL) return;
                        if (choice == IDYES) restoredSession = std::move(parsed);
                        else resumeNotice = L"已开始新盲评场次；现有恢复文件会在新状态保存时替换。";
                    }
                }
            } catch (const std::exception&) {
                const int choice = MessageBoxW(window_,
                    L"找到此报告的本机评审状态，但无法解密或验证。\n\n是：开始新的随机盲评；新状态保存后会替换旧文件。\n否：取消导入，保留当前窗口状态。",
                    L"无法恢复盲评场次", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (choice != IDYES) return;
                resumeNotice = L"旧盲评状态无法读取，已建立新场次；新状态保存后会替换旧文件。";
            }
        }
        if (blindReview && restoredSession) {
            randomizationSeedWords = restoredSession->seedWords;
            for (std::size_t index = 0; index < candidates.size(); ++index)
                candidates[index].ratings = std::move(restoredSession->ratingsBySource[index]);
            std::vector<NativeQualityReviewCandidate> ordered;
            ordered.reserve(candidates.size());
            for (const auto sourceIndex : restoredSession->candidateOrder)
                ordered.push_back(std::move(candidates[sourceIndex]));
            candidates = std::move(ordered);
        } else if (blindReview) {
            std::random_device entropy;
            randomizationSeedWords = std::array<std::uint32_t, 4>{
                static_cast<std::uint32_t>(entropy()), static_cast<std::uint32_t>(entropy()),
                static_cast<std::uint32_t>(entropy()), static_cast<std::uint32_t>(entropy())};
            std::seed_seq seed(randomizationSeedWords->begin(), randomizationSeedWords->end());
            std::mt19937 random(seed);
            std::shuffle(candidates.begin(), candidates.end(), random);
        }
        if (blindReview) for (std::size_t index = 0; index < candidates.size(); ++index) {
            std::wostringstream label;
            label << L"盲评候选 " << std::setfill(L'0') << std::setw(2) << (index + 1);
            candidates[index].blindLabel = label.str();
        }

        qualityReportPath_ = path; qualityReference_ = reference; qualityReferenceSha256_ = referenceSha256;
        qualityReportFingerprint_ = reportFingerprint;
        qualityReviewSessionId_ = restoredSession ? restoredSession->sessionId : newQualityReviewSessionId();
        blindRandomSeedWords_ = randomizationSeedWords;
        reviewerCode_ = restoredSession ? restoredSession->reviewerCode : std::wstring{};
        qualityReferenceIdentity_.reset(); qualityReferenceOrigin_.reset(); reviewCandidates_ = std::move(candidates);
        mediaHashesVerified_ = false; integrityChecking_ = false;
        blindReviewAvailable_ = blindReview;
        blindReviewMode_ = blindReview && (!restoredSession || restoredSession->blindReviewMode);
        blindReviewRevealed_ = blindReview && restoredSession && !restoredSession->blindReviewMode;
        selectedCandidate_ = 0;
        if (restoredSession) {
            const auto selected = std::find_if(reviewCandidates_.begin(), reviewCandidates_.end(), [&](const auto& candidate) {
                return candidate.sourceOrder == restoredSession->selectedSourceCandidate;
            });
            if (selected != reviewCandidates_.end())
                selectedCandidate_ = static_cast<std::size_t>(std::distance(reviewCandidates_.begin(), selected));
        }
        if (!populateCandidateList()) {
            clearQualityReview();
            throw std::runtime_error("无法显示质量报告候选列表");
        }
        layout();
        activateCandidate(selectedCandidate_, restoredSession ? restoredSession->selectedReviewPoint : std::optional<std::size_t>{},
            restoredSession ? std::optional<Rational>{restoredSession->position} : std::optional<Rational>{});
        if (blindReview) {
            if (resumeSaveFailed_) setStatus(L"本机加密恢复状态保存失败；请导出评分以免关闭后丢失。" + reviewCaption());
            else if (restoredSession) setStatus(L"已恢复本机加密盲评场次、候选顺序、评分和复核位置。" + reviewCaption());
            else if (!resumeNotice.empty()) setStatus(resumeNotice + reviewCaption());
            else setStatus(L"已建立随机盲评场次；本机恢复状态使用 Windows 当前用户加密保存。" + reviewCaption());
        }
    }
    [[nodiscard]] std::wstring candidateDisplayName(const NativeQualityReviewCandidate& candidate) const {
        return blindReviewMode_ ? candidate.blindLabel : candidate.name;
    }
    bool populateCandidateList() {
        const HWND control = GetDlgItem(window_, candidateId);
        SendMessageW(control, CB_RESETCONTENT, 0, 0);
        for (const auto& candidate : reviewCandidates_) {
            const auto label = candidateDisplayName(candidate);
            const LRESULT index = SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (index == CB_ERR || index == CB_ERRSPACE) { SendMessageW(control, CB_RESETCONTENT, 0, 0); return false; }
        }
        if (!reviewCandidates_.empty())
            SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(std::min(selectedCandidate_, reviewCandidates_.size() - 1)), 0);
        return true;
    }
    void revealQualityCandidates() {
        if (!blindReviewMode_ || integrityChecking_ || !window_) return;
        if (MessageBoxW(window_, L"结束匿名复核并显示候选真实名称及结果路径？", L"揭示质量评测候选",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        blindReviewMode_ = false;
        if (!populateCandidateList()) {
            blindReviewMode_ = true;
            (void)populateCandidateList();
            setStatus(L"无法揭示候选名称；盲评映射仍保持隐藏。" + reviewCaption());
            updateControls();
            return;
        }
        blindReviewRevealed_ = true;
        SetWindowTextW(GetDlgItem(window_, outputPathId), output_.c_str());
        setStatus(persistQualityReviewResume()
            ? L"匿名复核结束，候选映射已显示；揭示状态已加密保存。" + reviewCaption()
            : L"匿名复核结束，候选映射已显示；本机恢复状态保存失败。" + reviewCaption());
        updateControls();
    }
    void loadCurrentRatingControls() {
        if (!window_) return;
        LRESULT naturalnessSelection = 0, artifactSelection = 0, confidenceSelection = 0;
        std::wstring note;
        if (selectedCandidate_ < reviewCandidates_.size() && reviewCursor_ &&
            *reviewCursor_ < reviewCandidates_[selectedCandidate_].ratings.size()) {
            const auto& rating = reviewCandidates_[selectedCandidate_].ratings[*reviewCursor_];
            if (rating.recorded) {
                naturalnessSelection = rating.naturalness == -1 ? 6 : std::clamp(rating.naturalness, 1, 5);
                artifactSelection = std::clamp(rating.artifact, 0, 6) + 1;
                confidenceSelection = std::clamp(rating.confidence, 0, 5);
                note = rating.note;
            }
        }
        SendMessageW(GetDlgItem(window_, naturalnessRatingId), CB_SETCURSEL, static_cast<WPARAM>(naturalnessSelection), 0);
        SendMessageW(GetDlgItem(window_, artifactRatingId), CB_SETCURSEL, static_cast<WPARAM>(artifactSelection), 0);
        SendMessageW(GetDlgItem(window_, confidenceRatingId), CB_SETCURSEL, static_cast<WPARAM>(confidenceSelection), 0);
        SetWindowTextW(GetDlgItem(window_, reviewerNoteId), note.c_str());
    }
    void recordCurrentRating() {
        if (integrityChecking_ || busy_ || !ready() || !mediaHashesVerified_ ||
            selectedCandidate_ >= reviewCandidates_.size() || !reviewCursor_ ||
            *reviewCursor_ >= reviewCandidates_[selectedCandidate_].points.size()) return;
        const LRESULT naturalnessSelection = SendMessageW(GetDlgItem(window_, naturalnessRatingId), CB_GETCURSEL, 0, 0);
        const LRESULT artifactSelection = SendMessageW(GetDlgItem(window_, artifactRatingId), CB_GETCURSEL, 0, 0);
        if (naturalnessSelection < 1 || naturalnessSelection > 6 || artifactSelection < 1 || artifactSelection > 7) {
            setStatus(L"请选择自然度和伪影情况后再记录。" + reviewCaption());
            return;
        }
        auto& candidate = reviewCandidates_[selectedCandidate_];
        if (candidate.ratings.size() != candidate.points.size()) candidate.ratings.resize(candidate.points.size());
        auto& rating = candidate.ratings[*reviewCursor_];
        const int noteLength = GetWindowTextLengthW(GetDlgItem(window_, reviewerNoteId));
        std::wstring note(static_cast<std::size_t>(std::max(0, noteLength)) + 1, L'\0');
        const int copiedNoteLength = GetWindowTextW(GetDlgItem(window_, reviewerNoteId), note.data(), noteLength + 1);
        note.resize(static_cast<std::size_t>(std::max(0, copiedNoteLength)));
        if (note.find_first_not_of(L" \t\r\n") == std::wstring::npos) note.clear();
        rating.recorded = true;
        rating.naturalness = naturalnessSelection == 6 ? -1 : static_cast<int>(naturalnessSelection);
        rating.artifact = static_cast<int>(artifactSelection - 1);
        const LRESULT confidenceSelection = SendMessageW(GetDlgItem(window_, confidenceRatingId), CB_GETCURSEL, 0, 0);
        rating.confidence = confidenceSelection >= 1 && confidenceSelection <= 5 ? static_cast<int>(confidenceSelection) : 0;
        rating.note = std::move(note);
        setStatus(persistQualityReviewResume()
            ? L"已记录此复核点的自然度、伪影和可选评审备注；本机恢复状态已加密保存。" + reviewCaption()
            : L"评分已记录，但本机加密恢复保存失败；请导出评分以免关闭后丢失。" + reviewCaption());
        updateControls();
    }
    void editReviewerCode() {
        if (!window_ || reviewCandidates_.empty() || integrityChecking_) return;
        const bool hasRatings = std::any_of(reviewCandidates_.begin(), reviewCandidates_.end(), [](const auto& candidate) {
            return std::any_of(candidate.ratings.begin(), candidate.ratings.end(), [](const auto& rating) { return rating.recorded; });
        });
        if (hasRatings) {
            setStatus(L"匿名评审代号已在首条评分后锁定；如需更换评审者，请重新导入报告建立独立场次。" + reviewCaption());
            return;
        }
        ReviewerCodeDialogState state{reviewerCode_};
        auto dialogWords = reviewerCodeDialogTemplate();
        const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
            reinterpret_cast<const DLGTEMPLATE*>(dialogWords.data()), window_, reviewerCodeDialogProcedure,
            reinterpret_cast<LPARAM>(&state));
        if (result == -1) {
            setStatus(L"无法打开匿名评审代号设置窗口。" + reviewCaption());
            return;
        }
        if (result != IDOK) return;
        reviewerCode_ = std::move(state.value);
        const bool saved = persistQualityReviewResume();
        setStatus(reviewerCode_.empty()
            ? (saved ? L"已清除匿名评审代号；导出不会自动分组合并同一评审者的多个场次。" : L"已清除匿名评审代号，但本机恢复状态保存失败。") + reviewCaption()
            : (saved ? L"已设置匿名评审代号；导出会保留此化名供多场次汇总，但无法验证真实身份。" : L"已设置匿名评审代号，但本机恢复状态保存失败。") + reviewCaption());
    }
    [[nodiscard]] std::wstring qualityReviewExportLabel(std::size_t index) const {
        if (index < reviewCandidates_.size() && !reviewCandidates_[index].blindLabel.empty())
            return reviewCandidates_[index].blindLabel;
        std::wostringstream label;
        label << L"候选 " << std::setfill(L'0') << std::setw(2) << (index + 1);
        return label.str();
    }
    [[nodiscard]] std::wstring qualityReviewExportName(const NativeQualityReviewCandidate& candidate, std::size_t index) const {
        auto name = candidate.name;
        if (name.find_first_of(L"/\\") != std::wstring::npos || (name.size() >= 2 && name[1] == L':'))
            name = std::filesystem::path(name).filename().wstring();
        if (name.empty()) name = qualityReviewExportLabel(index);
        return name;
    }
    void chooseRatingExport() {
        if (!window_ || reviewCandidates_.empty()) return;
        std::size_t recordedCount = 0;
        for (const auto& candidate : reviewCandidates_)
            recordedCount += static_cast<std::size_t>(std::count_if(candidate.ratings.begin(), candidate.ratings.end(),
                [](const auto& rating) { return rating.recorded; }));
        if (recordedCount == 0) {
            setStatus(L"还没有已记录的复核评分。" + reviewCaption());
            return;
        }

        static constexpr std::array<const wchar_t*, 7> artifactLabels{
            L"无明显伪影", L"运动撕裂或错位", L"重影或拖影", L"闪烁或时间不稳",
            L"纹理或细节失真", L"遮挡边界错误", L"其他明显伪影"};
        static constexpr std::array<const char*, 7> artifactCodes{
            "none_obvious", "motion_tearing_or_misalignment", "ghosting_or_smearing", "flicker_or_temporal_instability",
            "texture_or_detail_distortion", "occlusion_boundary_error", "other_obvious_artifact"};
        static constexpr std::array<const wchar_t*, 6> naturalnessLabels{
            L"无法判断", L"1：明显不自然", L"2：略不自然", L"3：自然度一般", L"4：较自然", L"5：非常自然"};

        nlohmann::json document;
        document["schemaVersion"] = 1;
        document["application"] = "RifeLove";
        document["recordType"] = "human_visual_review";
        document["sessionId"] = qualityReviewSessionId_;
        document["createdAtUnixMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        document["blindReview"] = blindReviewAvailable_;
        document["identitiesIncluded"] = !blindReviewMode_;
        if (!reviewerCode_.empty()) document["reviewerCode"] = nativeModelUtf8(reviewerCode_);
        document["ratings"] = nlohmann::json::array();
        for (std::size_t candidateIndex = 0; candidateIndex < reviewCandidates_.size(); ++candidateIndex) {
            const auto& candidate = reviewCandidates_[candidateIndex];
            const auto label = nativeModelUtf8(qualityReviewExportLabel(candidateIndex));
            const auto pointCount = std::min(candidate.points.size(), candidate.ratings.size());
            for (std::size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
                const auto& rating = candidate.ratings[pointIndex];
                if (!rating.recorded) continue;
                const auto& point = candidate.points[pointIndex];
                const int naturalnessIndex = rating.naturalness < 0 ? 0 : std::clamp(rating.naturalness, 1, 5);
                const int artifactIndex = std::clamp(rating.artifact, 0, 6);
                nlohmann::json entry;
                entry["candidateLabel"] = label;
                entry["reviewPointIndex"] = pointIndex + 1;
                entry["pointType"] = point.transition ? "transition" : "frame";
                entry["relativeTime"] = point.relativeTime.toString();
                if (point.endRelativeTime) entry["endRelativeTime"] = point.endRelativeTime->toString();
                entry["naturalnessScore"] = rating.naturalness < 0 ? nlohmann::json(nullptr) : nlohmann::json(naturalnessIndex);
                entry["unableToJudge"] = rating.naturalness < 0;
                entry["naturalnessLabel"] = nativeModelUtf8(naturalnessLabels[static_cast<std::size_t>(naturalnessIndex)]);
                entry["observedArtifact"] = artifactCodes[static_cast<std::size_t>(artifactIndex)];
                entry["observedArtifactLabel"] = nativeModelUtf8(artifactLabels[static_cast<std::size_t>(artifactIndex)]);
                if (rating.confidence >= 1 && rating.confidence <= 5) entry["confidenceScore"] = rating.confidence;
                if (!rating.note.empty()) entry["note"] = nativeModelUtf8(rating.note);
                document["ratings"].push_back(std::move(entry));
            }
        }
        if (!blindReviewMode_) {
            document["candidateMapping"] = nlohmann::json::array();
            if (blindReviewAvailable_ && blindRandomSeedWords_) {
                document["randomization"] = nlohmann::json::object();
                document["randomization"]["algorithm"] = "std::mt19937 + std::shuffle";
                document["randomization"]["seedWords"] = nlohmann::json::array();
                for (const auto seedWord : *blindRandomSeedWords_)
                    document["randomization"]["seedWords"].push_back(seedWord);
            }
            for (std::size_t index = 0; index < reviewCandidates_.size(); ++index) {
                const auto& candidate = reviewCandidates_[index];
                document["candidateMapping"].push_back({
                    {"candidateLabel", nativeModelUtf8(qualityReviewExportLabel(index))},
                    {"sourceCandidateIndex", candidate.sourceOrder},
                    {"name", nativeModelUtf8(qualityReviewExportName(candidate, index))},
                    {"outputSha256", candidate.outputSha256}});
            }
        }

        wchar_t selectedPath[32768]{};
        const std::wstring defaultName = blindReviewMode_ ? L"RifeLove-blind-review.json" : L"RifeLove-review.json";
        std::copy(defaultName.begin(), defaultName.end(), selectedPath);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFile = selectedPath; dialog.nMaxFile = static_cast<DWORD>(std::size(selectedPath));
        dialog.lpstrFilter = L"RifeLove 评审记录 (*.json)\0*.json\0所有文件\0*.*\0\0";
        dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = blindReviewMode_ ? L"导出匿名盲评记录" : L"导出评审记录";
        if (!GetSaveFileNameW(&dialog)) return;

        HANDLE output = INVALID_HANDLE_VALUE;
        std::filesystem::path temporary;
        bool temporaryCreated = false;
        try {
            const auto path = std::filesystem::absolute(std::filesystem::path(selectedPath)).lexically_normal();
            (void)checkedNativeModelDirectory(path.parent_path(), false);
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("评审记录必须写入本地普通文件，不能覆盖链接或目录");
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD inspectError = GetLastError();
                if (inspectError != ERROR_FILE_NOT_FOUND && inspectError != ERROR_PATH_NOT_FOUND)
                    throw std::runtime_error("无法检查评审记录目标文件");
            }
            const auto serialized = document.dump(2) + "\n";
            if (serialized.size() > 16 * 1024 * 1024) throw std::runtime_error("评审记录超过 16 MiB 限制");
            static std::atomic<unsigned long> sequence{0};
            temporary = path;
            temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence++);
            output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建临时评审记录文件");
            temporaryCreated = true;
            DWORD written = 0;
            if (!WriteFile(output, serialized.data(), static_cast<DWORD>(serialized.size()), &written, nullptr) ||
                written != static_cast<DWORD>(serialized.size()) || !FlushFileBuffers(output))
                throw std::runtime_error("写入评审记录失败");
            CloseHandle(output); output = INVALID_HANDLE_VALUE;
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("无法保存评审记录");
            temporary.clear();
            temporaryCreated = false;
            setStatus(L"已导出 " + std::to_wstring(recordedCount) + L" 条" +
                (blindReviewMode_ ? L"匿名盲评记录；文件不包含候选身份、路径或模型指标。" : L"评审记录，包含候选名称与输出哈希。") +
                (reviewerCode_.empty() ? L"" : L" 已包含匿名评审代号。") + reviewCaption());
        } catch (const std::exception& error) {
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (temporaryCreated && !temporary.empty()) DeleteFileW(temporary.c_str());
            setStatus(L"导出评审记录失败：" + wide(error.what()) + reviewCaption());
        }
    }
    void activateCandidate(std::size_t index, std::optional<std::size_t> resumePoint = {},
        std::optional<Rational> resumePosition = {}) {
        if (integrityChecking_ || index >= reviewCandidates_.size()) return;
        selectedCandidate_ = index;
        SendMessageW(GetDlgItem(window_, candidateId), CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        const auto& candidate = reviewCandidates_[index];
        mediaHashesVerified_ = qualityReferenceIdentity_.has_value() && candidate.verifiedIdentity.has_value();
        if (candidate.points.empty()) reviewCursor_.reset();
        else if (resumePoint && *resumePoint < candidate.points.size()) reviewCursor_ = *resumePoint;
        else if (resumePosition) reviewCursor_.reset();
        else reviewCursor_ = 0;
        loadCurrentRatingControls();
        setFilesInternal(qualityReference_, candidate.output, false,
            resumePosition ? resumePosition : candidate.points.empty() ? std::optional<Rational>{} :
                std::optional<Rational>{candidate.points[reviewCursor_.value_or(0)].relativeTime});
        updateControls();
        if (!persistQualityReviewResume())
            setStatus(L"候选已切换，但本机加密恢复保存失败；请导出评分以免关闭后丢失。" + reviewCaption());
    }
    void navigateReview(int direction) {
        if (integrityChecking_ || direction == 0 || selectedCandidate_ >= reviewCandidates_.size()) return;
        const auto& points = reviewCandidates_[selectedCandidate_].points;
        if (points.empty()) return;
        std::size_t next = 0;
        if (reviewCursor_) {
            if (direction < 0) {
                if (*reviewCursor_ == 0) return;
                next = *reviewCursor_ - 1;
            } else {
                if (*reviewCursor_ + 1 >= points.size()) return;
                next = *reviewCursor_ + 1;
            }
        } else if (pair_) {
            const auto position = pair_->requested;
            if (direction < 0) {
                const auto found = std::lower_bound(points.begin(), points.end(), position,
                    [](const auto& point, Rational time) { return point.relativeTime < time; });
                if (found == points.begin()) return;
                next = static_cast<std::size_t>(std::distance(points.begin(), found) - 1);
            } else {
                const auto found = std::upper_bound(points.begin(), points.end(), position,
                    [](Rational time, const auto& point) { return time < point.relativeTime; });
                if (found == points.end()) return;
                next = static_cast<std::size_t>(std::distance(points.begin(), found));
            }
        } else if (direction > 0) next = 0;
        else next = points.size() - 1;
        reviewCursor_ = next;
        loadCurrentRatingControls();
        seekInternal(points[next].relativeTime, {}, true);
        updateControls();
        if (!persistQualityReviewResume())
            setStatus(L"复核位置已更新，但本机加密恢复保存失败；请导出评分以免关闭后丢失。" + reviewCaption());
    }
    int px(int value) const { const UINT dpi = window_ ? GetDpiForWindow(window_) : 96; return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); }
    void invalidateRequests(bool detach = true) {
        std::lock_guard lock(shared_->mutex);
        ++shared_->generation; shared_->active.request_stop(); shared_->pending.reset(); shared_->completed.reset();
        if (detach) shared_->target = nullptr;
    }
    void setStatus(const std::wstring& text) { if (window_) SetWindowTextW(GetDlgItem(window_, statusId), text.c_str()); }
    std::wstring reviewCaption() const {
        if (qualityReportPath_.empty() || selectedCandidate_ >= reviewCandidates_.size()) return {};
        const auto& candidate = reviewCandidates_[selectedCandidate_];
        std::wostringstream text;
        text << L" · " << (blindReviewMode_ ? std::wstring(L"匿名质量报告") : qualityReportPath_.filename().wstring())
             << L" · " << candidateDisplayName(candidate)
             << L" · 评测指标仅作复核线索";
        if (blindReviewMode_) text << L" · 匿名盲评";
        else if (reviewCandidates_.size() > 1 && !blindReviewAvailable_)
            text << L"；此报告缺少共用复核点，重新评测后可启用盲评";
        if (mediaHashesVerified_) text << L"；本候选与参考 SHA-256 已匹配，后续切换或定位会检查文件身份";
        else if (!integrityChecking_) text << L"；首次加载时会核对媒体 SHA-256";
        if (reviewCursor_ && *reviewCursor_ < candidate.points.size()) {
            const auto& point = candidate.points[*reviewCursor_];
            text << L" · 复核点 " << (*reviewCursor_ + 1) << L"/" << candidate.points.size()
                 << (point.transition ? L" 过渡 " : L" 帧 ") << seconds(point.relativeTime);
            if (point.endRelativeTime) text << L"–" << seconds(*point.endRelativeTime);
            if (blindReviewMode_) text << L" · 盲评期间隐藏指标";
            else {
                text << L" · " << (point.transition ? L"差分 PSNR " : L"PSNR ");
                if (point.psnrPerfect) text << L"完美匹配";
                else text << std::fixed << std::setprecision(3) << point.psnrDb << L" dB";
                text << L" · SSIM " << point.ssim;
            }
            const bool recorded = *reviewCursor_ < candidate.ratings.size() && candidate.ratings[*reviewCursor_].recorded;
            text << (recorded ? L" · 已评分" : L" · 待评分");
        }
        const auto recordedPoints = std::count_if(candidate.ratings.begin(), candidate.ratings.end(),
            [](const auto& rating) { return rating.recorded; });
        text << L" · 已记录 " << recordedPoints << L"/" << candidate.points.size();
        if (pair_ && pair_->sourceLease && pair_->outputLease)
            text << L"；当前画面保持媒体只读，关闭比较窗口后释放";
        return text.str();
    }
    void updateIntegrityProgress() {
        if (!window_ || !integrityChecking_ || !hashProgress_) {
            if (window_) KillTimer(window_, integrityTimer);
            return;
        }
        const auto total = hashProgress_->total.load(std::memory_order_relaxed);
        const auto completed = hashProgress_->completed.load(std::memory_order_relaxed);
        std::wstring progress;
        if (total == 0) progress = L"正在准备核对评测媒体 SHA-256";
        else if (completed >= total) progress = L"媒体 SHA-256 已核对，正在读取对照帧";
        else {
            const auto percent = static_cast<unsigned>(std::clamp(
                static_cast<long double>(completed) * 100.0L / static_cast<long double>(total), 0.0L, 100.0L));
            progress = L"正在核对评测报告中的媒体 SHA-256：" + std::to_wstring(percent) + L"%";
        }
        setStatus(progress + L" · 可关闭窗口取消" + reviewCaption());
    }
    void updateReviewControls() {
        if (!window_) return;
        const bool candidates = !reviewCandidates_.empty();
        const int ratingVisibility = candidates ? SW_SHOWNA : SW_HIDE;
        ShowWindow(GetDlgItem(window_, naturalnessRatingId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, artifactRatingId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, confidenceRatingId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, reviewerNoteId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, recordRatingId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, reviewerCodeButtonId), ratingVisibility);
        ShowWindow(GetDlgItem(window_, exportRatingsId), candidates ? SW_SHOWNA : SW_HIDE);
        EnableWindow(GetDlgItem(window_, candidateId), candidates && !integrityChecking_);
        bool canPrevious = false, canNext = false;
        if (selectedCandidate_ < reviewCandidates_.size() && !reviewCandidates_[selectedCandidate_].points.empty()) {
            const auto& points = reviewCandidates_[selectedCandidate_].points;
            if (reviewCursor_ && *reviewCursor_ < points.size()) {
                canPrevious = *reviewCursor_ > 0;
                canNext = *reviewCursor_ + 1 < points.size();
            } else {
                canPrevious = canNext = true;
            }
        }
        EnableWindow(GetDlgItem(window_, previousReviewId), canPrevious && !integrityChecking_);
        EnableWindow(GetDlgItem(window_, nextReviewId), canNext && !integrityChecking_);
        const bool hasReviewPoint = selectedCandidate_ < reviewCandidates_.size() && reviewCursor_ &&
            *reviewCursor_ < reviewCandidates_[selectedCandidate_].points.size();
        const bool canRate = hasReviewPoint && mediaHashesVerified_ && !integrityChecking_ && !busy_ && ready();
        EnableWindow(GetDlgItem(window_, naturalnessRatingId), canRate);
        EnableWindow(GetDlgItem(window_, artifactRatingId), canRate);
        EnableWindow(GetDlgItem(window_, confidenceRatingId), canRate);
        EnableWindow(GetDlgItem(window_, reviewerNoteId), canRate);
        const LRESULT naturalnessSelection = SendMessageW(GetDlgItem(window_, naturalnessRatingId), CB_GETCURSEL, 0, 0);
        const LRESULT artifactSelection = SendMessageW(GetDlgItem(window_, artifactRatingId), CB_GETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(window_, recordRatingId), canRate && naturalnessSelection >= 1 && naturalnessSelection <= 6 &&
            artifactSelection >= 1 && artifactSelection <= 7);
        const bool hasRatings = std::any_of(reviewCandidates_.begin(), reviewCandidates_.end(), [](const auto& candidate) {
            return std::any_of(candidate.ratings.begin(), candidate.ratings.end(), [](const auto& rating) { return rating.recorded; });
        });
        EnableWindow(GetDlgItem(window_, reviewerCodeButtonId), candidates && !integrityChecking_ && !hasRatings);
        EnableWindow(GetDlgItem(window_, exportRatingsId), candidates && hasRatings);
    }
    void updateControls() {
        if (!window_) return;
        EnableWindow(GetDlgItem(window_, sourceBrowseId), !integrityChecking_);
        EnableWindow(GetDlgItem(window_, outputBrowseId), !integrityChecking_);
        EnableWindow(GetDlgItem(window_, reportBrowseId), !integrityChecking_);
        EnableWindow(GetDlgItem(window_, timeId), !integrityChecking_);
        EnableWindow(GetDlgItem(window_, playId), !integrityChecking_ && (playing_ || ready()));
        SetWindowTextW(GetDlgItem(window_, playId), playing_ ? L"暂停" : L"播放 / 静音");
        EnableWindow(GetDlgItem(window_, previousId), !integrityChecking_ && ready() && (pair_->output.previous || pair_->output.timestamp > pair_->outputOrigin));
        EnableWindow(GetDlgItem(window_, nextId), !integrityChecking_ && ready() && pair_->output.next.has_value());
        EnableWindow(GetDlgItem(window_, sliderId), !integrityChecking_ && pair_ && pair_->duration && *pair_->duration > Rational(0));
        EnableWindow(GetDlgItem(window_, goId), !integrityChecking_ && !source_.empty() && !output_.empty());
        SetWindowTextW(GetDlgItem(window_, revealQualityCandidatesId),
            blindReviewMode_ ? L"揭示候选" : blindReviewAvailable_ && blindReviewRevealed_ ? L"名称已揭示" :
            reviewCandidates_.size() > 1 ? L"报告需更新" : L"盲评需多候选");
        EnableWindow(GetDlgItem(window_, revealQualityCandidatesId), blindReviewMode_ && !integrityChecking_);
        SetWindowTextW(GetDlgItem(window_, exportRatingsId), L"导出评审");
        updateReviewControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void showPairStatus(bool ended = false) {
        if (!pair_ || !error_.empty()) return;
        const auto position = pair_->output.timestamp - pair_->outputOrigin;
        const auto playback = playing_ ? std::wstring(L"同步播放 · 静音 · ") :
            ended ? std::wstring(L"播放结束 · 静音 · ") : std::wstring(L"已暂停 · 静音 · ");
        if (!playbackTelemetry_.empty()) {
            setStatus(playback + L"结果 " + seconds(position) + L" · " + playbackTelemetry_ +
                L" · 解码落后时跳过过期帧。" + reviewCaption());
            return;
        }
        const auto preview = shared_->legacyPlayback
            ? L"预览最高 30 FPS，解码较慢时跳过预览帧；"
            : L"按呈现时间戳同步；解码落后时跳过过期预览帧；";
        setStatus(playback +
            L"请求 " + seconds(pair_->requested) + L" · 原片 " + seconds(pair_->source.timestamp - pair_->sourceOrigin) +
            L" · 结果 " + seconds(position) + L" · " + preview + L"逐帧可查看全部帧。未知像素比例时按编码像素显示。" + reviewCaption());
    }
    bool updatePlaybackTelemetry(bool force = false) {
        if (shared_->legacyPlayback || !playbackMetrics_) return false;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(now - playbackStatsWindow_).count();
        if (elapsed <= 0 || (!force && elapsed < 0.5) || (force && elapsed < 0.1)) return false;
        const auto sourceDecoded = playbackMetrics_->sourceDecodedFrames.load(std::memory_order_relaxed);
        const auto outputDecoded = playbackMetrics_->outputDecodedFrames.load(std::memory_order_relaxed);
        const auto synchronized = playbackMetrics_->synchronizedPairs.load(std::memory_order_relaxed);
        const auto rate = [elapsed](std::uint64_t current, std::uint64_t previous) {
            return static_cast<double>(current - previous) / elapsed;
        };
        std::wostringstream text;
        text << std::fixed << std::setprecision(1)
            << L"实测解码 原片 " << rate(sourceDecoded, lastSourceDecodedSample_)
            << L" / 结果 " << rate(outputDecoded, lastOutputDecodedSample_) << L" fps"
            << L" · 配对 " << rate(synchronized, lastSynchronizedSample_)
            << L" · 界面更新 " << rate(playbackDisplayedPairs_, lastDisplayedPairsSample_) << L" fps"
            << L" · 跳过 " << playbackSkippedPairs_;
        playbackTelemetry_ = text.str();
        lastSourceDecodedSample_ = sourceDecoded;
        lastOutputDecodedSample_ = outputDecoded;
        lastSynchronizedSample_ = synchronized;
        lastDisplayedPairsSample_ = playbackDisplayedPairs_;
        playbackStatsWindow_ = now;
        return true;
    }
    void updatePlaybackDisplay(bool telemetryChanged) {
        if (!window_ || !pair_) return;
        const auto now = std::chrono::steady_clock::now();
        const auto position = pair_->output.timestamp - pair_->outputOrigin;
        const auto value = wide(position.toString());
        SetWindowTextW(GetDlgItem(window_, timeId), value.c_str());
        if (pair_->duration && *pair_->duration > Rational(0) &&
            now - lastPlaybackSliderUpdate_ >= std::chrono::milliseconds(100)) {
            SetScrollPos(GetDlgItem(window_, sliderId), SB_CTL, static_cast<int>(std::clamp(
                (position / *pair_->duration).toLongDouble() * 10000.0L, 0.0L, 10000.0L)), TRUE);
            lastPlaybackSliderUpdate_ = now;
        }
        if (telemetryChanged || now - lastPlaybackDetailsUpdate_ >= std::chrono::milliseconds(500)) {
            showPairStatus();
            lastPlaybackDetailsUpdate_ = now;
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    void playbackTick() {
        if (!playing_ || busy_ || !ready()) return;
        try {
            if (shared_->legacyPlayback) {
                if (!playbackClockReady_) return;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - playbackStarted_).count();
                auto time = playbackAnchor_ + Rational(std::max<std::int64_t>(0, elapsed), 1000);
                if (pair_->duration) time = std::min(time, *pair_->duration);
                if (time > pair_->requested) requestPair(time);
                return;
            }
            std::wstring playbackError;
            bool finished = false;
            {
                std::lock_guard lock(shared_->mutex);
                if (!playbackClockReady_ && !shared_->playbackFrames.empty()) {
                    playbackStarted_ = std::chrono::steady_clock::now();
                    playbackClockReady_ = true;
                }
                finished = shared_->playbackDone && shared_->playbackFrames.empty();
                playbackError = shared_->playbackError;
                if (!playbackClockReady_ && !finished) return;
            }

            Rational time = playbackAnchor_;
            if (playbackClockReady_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - playbackStarted_).count();
                time = playbackAnchor_ + Rational(std::max<std::int64_t>(0, elapsed), 1000000);
            }
            if (pair_->duration) time = std::min(time, *pair_->duration);
            std::optional<NativeComparisonPair> latest;
            std::uint64_t consumedPairs = 0;
            {
                std::lock_guard lock(shared_->mutex);
                while (!shared_->playbackFrames.empty() && shared_->playbackFrames.front().requested <= time) {
                    latest = std::move(shared_->playbackFrames.front());
                    shared_->playbackFrames.pop_front();
                    ++consumedPairs;
                }
                finished = shared_->playbackDone && shared_->playbackFrames.empty();
                playbackError = shared_->playbackError;
            }
            shared_->playbackWake.notify_all();

            if (latest) {
                pair_ = std::move(latest);
                ++playbackDisplayedPairs_;
                if (consumedPairs > 1) playbackSkippedPairs_ += consumedPairs - 1;
            }
            const bool telemetryChanged = updatePlaybackTelemetry(finished);
            if (latest) updatePlaybackDisplay(telemetryChanged);
            else if (telemetryChanged) {
                showPairStatus();
                lastPlaybackDetailsUpdate_ = std::chrono::steady_clock::now();
            }
            if (finished) {
                playing_ = false;
                KillTimer(window_, playbackTimer);
                if (!playbackError.empty())
                    setStatus(L"同步预览已停止，保留最后一组画面：" + playbackError + reviewCaption());
                else
                    showPairStatus(true);
                updateControls();
            }
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
        busy_ = false; integrityChecking_ = false; hashProgress_.reset(); KillTimer(window_, integrityTimer);
        error_ = std::move(result->error); pair_ = std::move(result->pair);
        if (!error_.empty() || !pair_) {
            playing_ = false; KillTimer(window_, playbackTimer);
            pair_.reset(); if (error_.empty()) error_ = L"未返回比较帧";
            setStatus(L"无法显示比较帧：" + error_ + reviewCaption());
        } else {
            if (pair_->reportHashesMatched && pair_->sourceIdentity && pair_->outputIdentity &&
                selectedCandidate_ < reviewCandidates_.size()) {
                qualityReferenceIdentity_ = pair_->sourceIdentity;
                reviewCandidates_[selectedCandidate_].verifiedIdentity = pair_->outputIdentity;
                qualityReferenceOrigin_ = pair_->sourceOrigin;
                reviewCandidates_[selectedCandidate_].verifiedOrigin = pair_->outputOrigin;
                mediaHashesVerified_ = true;
            }
            const auto position = pair_->output.timestamp - pair_->outputOrigin;
            const auto value = wide(position.toString());
            SetWindowTextW(GetDlgItem(window_, timeId), value.c_str());
            if (pair_->duration && *pair_->duration > Rational(0))
                SetScrollPos(GetDlgItem(window_, sliderId), SB_CTL, static_cast<int>(std::clamp((position / *pair_->duration).toLongDouble() * 10000.0L, 0.0L, 10000.0L)), TRUE);
            const bool ended = playing_ && (pair_->duration ? pair_->requested >= *pair_->duration :
                !pair_->source.next || !pair_->output.next);
            if (playing_ && !playbackClockReady_ && !ended) {
                playbackAnchor_ = pair_->requested;
                if (shared_->legacyPlayback) {
                    playbackStarted_ = std::chrono::steady_clock::now();
                    playbackClockReady_ = true;
                } else startPlayback();
            }
            if (ended) { playing_ = false; KillTimer(window_, playbackTimer); cancelPlayback(); }
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
        dialog.lpstrTitle = output ? (qualityReference_.empty() ? L"选择已完成的处理结果" : L"选择候选视频") :
            (qualityReference_.empty() ? L"选择原始视频" : L"选择高帧率参考");
        if (GetOpenFileNameW(&dialog)) setFiles(output ? source_ : std::filesystem::path(path), output ? std::filesystem::path(path) : output_);
    }
    static LRESULT CALLBACK comparisonComboProcedure(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                                      UINT_PTR, DWORD_PTR reference) {
        auto* self = reinterpret_cast<NativeComparisonWindow*>(reference);
        if (!self) return DefSubclassProc(control, message, wParam, lParam);
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(control, &paint) : reinterpret_cast<HDC>(wParam);
            if (!dc) return 0;
            const int saved = SaveDC(dc);
            RECT rect{}; GetClientRect(control, &rect);
            HBRUSH background = CreateSolidBrush(RGB(12, 15, 21));
            FillRect(dc, &rect, background); DeleteObject(background);
            const bool enabled = IsWindowEnabled(control) != FALSE;
            const bool focused = GetFocus() == control;
            HBRUSH fill = CreateSolidBrush(enabled ? RGB(20, 25, 34) : RGB(23, 28, 37));
            HPEN border = CreatePen(PS_SOLID, std::max(1, self->px(1)), focused ? RGB(162, 231, 203) : RGB(45, 55, 70));
            const auto previousBrush = SelectObject(dc, fill), previousPen = SelectObject(dc, border);
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, self->px(8), self->px(8));
            SelectObject(dc, previousPen); SelectObject(dc, previousBrush); DeleteObject(border); DeleteObject(fill);
            std::wstring label;
            const auto index = SendMessageW(control, CB_GETCURSEL, 0, 0);
            if (index != CB_ERR) {
                const auto length = SendMessageW(control, CB_GETLBTEXTLEN, index, 0);
                if (length > 0) {
                    label.resize(static_cast<std::size_t>(length) + 1);
                    SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()));
                    label.resize(static_cast<std::size_t>(length));
                }
            }
            if (label.empty() && GetDlgCtrlID(control) == candidateId)
                label = L"导入报告后选择候选";
            const COLORREF textColor = enabled ? RGB(230, 238, 247) : RGB(127, 142, 159);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, textColor);
            if (self->font_) SelectObject(dc, self->font_);
            RECT text = rect; text.left += self->px(12); text.right -= self->px(30);
            DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            POINT arrow[3]{{rect.right - self->px(18), (rect.top + rect.bottom) / 2 - self->px(2)},
                {rect.right - self->px(10), (rect.top + rect.bottom) / 2 - self->px(2)},
                {rect.right - self->px(14), (rect.top + rect.bottom) / 2 + self->px(3)}};
            HBRUSH arrowBrush = CreateSolidBrush(textColor);
            const auto oldArrow = SelectObject(dc, arrowBrush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            Polygon(dc, arrow, 3); SelectObject(dc, oldPen); SelectObject(dc, oldArrow); DeleteObject(arrowBrush);
            if (focused) { RECT focus = rect; InflateRect(&focus, -self->px(3), -self->px(3)); DrawFocusRect(dc, &focus); }
            RestoreDC(dc, saved);
            if (message == WM_PAINT) EndPaint(control, &paint);
            return 0;
        }
        if (message == CB_SETCURSEL) {
            const auto result = DefSubclassProc(control, message, wParam, lParam);
            InvalidateRect(control, nullptr, FALSE); return result;
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
            InvalidateRect(control, nullptr, FALSE);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(control, comparisonComboProcedure, 1);
        return DefSubclassProc(control, message, wParam, lParam);
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
        for (const int id : {candidateId, confidenceRatingId, naturalnessRatingId, artifactRatingId}) {
            const HWND combo = GetDlgItem(window_, id);
            SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(32));
            SendMessageW(combo, CB_SETITEMHEIGHT, 0, px(32));
        }
        move(sourcePathId, px(70), px(45), w - px(210), px(32));
        move(outputPathId, px(70), px(82), w - px(210), px(32));
        move(sourceBrowseId, w - px(128), px(43), px(108), px(36));
        move(outputBrowseId, w - px(128), px(80), px(108), px(36));
        move(reportBrowseId, px(70), px(117), px(118), px(36));
        const int reviewButtonWidth = px(94), reviewButtonGap = px(8);
        const int previousReviewX = w - px(20) - reviewButtonWidth * 2 - reviewButtonGap;
        move(candidateId, px(196), px(117), previousReviewX - px(204), px(180));
        move(previousReviewId, previousReviewX, px(117), reviewButtonWidth, px(36));
        move(nextReviewId, previousReviewX + reviewButtonWidth + reviewButtonGap, px(117), reviewButtonWidth, px(36));
        const int ratingActionX = w - px(128);
        move(confidenceRatingId, px(20), px(157), px(130), px(240));
        move(naturalnessRatingId, px(158), px(157), px(175), px(240));
        move(artifactRatingId, px(341), px(157), ratingActionX - px(349), px(280));
        move(recordRatingId, ratingActionX, px(157), px(108), px(36));
        move(reviewerNoteId, px(174), px(198), w - px(194), px(42));
        move(sliderId, px(20), h - px(compact ? 168 : 126), w - px(40), px(20));
        move(previousId, px(20), h - px(compact ? 136 : 94), px(96), px(34));
        move(nextId, px(124), h - px(compact ? 136 : 94), px(96), px(34));
        move(playId, px(228), h - px(compact ? 136 : 94), px(96), px(34));
        move(timeId, px(compact ? 20 : 340), h - px(92), px(128), px(30));
        move(goId, px(compact ? 156 : 476), h - px(94), px(80), px(34));
        move(closeId, w - px(116), h - px(compact ? 136 : 94), px(96), px(34));
        const int footerButtonWidth = px(96), footerButtonGap = px(6);
        const int exportX = w - px(20) - footerButtonWidth;
        const int reviewerCodeX = exportX - footerButtonGap - footerButtonWidth;
        const int revealX = reviewerCodeX - footerButtonGap - footerButtonWidth;
        move(statusId, px(20), h - px(49), revealX - px(28), px(42));
        move(revealQualityCandidatesId, revealX, h - px(49), footerButtonWidth, px(34));
        move(reviewerCodeButtonId, reviewerCodeX, h - px(49), footerButtonWidth, px(34));
        move(exportRatingsId, exportX, h - px(49), footerButtonWidth, px(34));
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
        text(qualityReference_.empty() ? L"原片" : L"参考", RECT{px(20), px(50), px(66), px(73)});
        text(qualityReference_.empty() ? L"结果" : L"候选", RECT{px(20), px(87), px(66), px(110)});
        text(L"报告", RECT{px(20), px(124), px(66), px(147)});
        if (!reviewCandidates_.empty())
            text(L"备注（盲评勿写名称）", RECT{px(20), px(202), px(168), px(235)});
        const bool compact = client.right < px(760);
        const int gap = px(14), inset = px(20), width = std::max(1, (static_cast<int>(client.right) - inset * 2 - gap) / 2);
        const std::array<const NativeComparisonImage*, 2> images{pair_ ? &pair_->source : nullptr, pair_ ? &pair_->output : nullptr};
        for (int side = 0; side < 2; ++side) {
            RECT pane{inset + side * (width + gap), px(reviewCandidates_.empty() ? 159 : 250),
                inset + side * (width + gap) + width, client.bottom - px(compact ? 184 : 142)};
            if (pane.bottom <= pane.top || pane.right <= pane.left) continue;
            FillRect(dc, &pane, field_);
            RECT title{pane.left + px(12), pane.top + px(10), pane.right - px(12), pane.top + px(32)};
            text(side == 0 ? (qualityReference_.empty() ? L"原片" : L"高帧率参考") :
                (qualityReference_.empty() ? L"处理结果" : L"候选视频"), title);
            RECT imageArea{pane.left + px(8), pane.top + px(58), pane.right - px(8), pane.bottom - px(8)};
            if (imageArea.right <= imageArea.left || imageArea.bottom <= imageArea.top) continue;
            const auto* image = images[static_cast<std::size_t>(side)];
            if ((!ready() && !(playing_ && pair_ && error_.empty())) || !image || !image->bgra) { text(busy_ ? L"正在读取…" : error_.empty() ? L"选择视频以显示画面" : L"当前帧不可用；请查看底部说明", imageArea, true); continue; }
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
            control(L"BUTTON", L"导入报告…", WS_TABSTOP | BS_OWNERDRAW, reportBrowseId);
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                0, 0, self->px(220), self->px(180), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(candidateId)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(GetDlgItem(window, candidateId), CB_SETMINVISIBLE, 8, 0);
            control(L"BUTTON", L"← 复核点", WS_TABSTOP | BS_OWNERDRAW, previousReviewId);
            control(L"BUTTON", L"复核点 →", WS_TABSTOP | BS_OWNERDRAW, nextReviewId);
            const HWND confidence = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 0, 0, self->px(130), self->px(180), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(confidenceRatingId)), GetModuleHandleW(nullptr), nullptr);
            const std::array<const wchar_t*, 6> confidenceOptions{
                L"信心度（可选）", L"1：很低", L"2：较低", L"3：一般", L"4：较高", L"5：很高"};
            for (const auto* option : confidenceOptions) SendMessageW(confidence, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
            SendMessageW(confidence, CB_SETMINVISIBLE, 6, 0);
            const HWND reviewerNote = control(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, reviewerNoteId);
            SendMessageW(reviewerNote, EM_LIMITTEXT, 1000, 0);
            const HWND naturalness = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 0, 0, self->px(240), self->px(200), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(naturalnessRatingId)), GetModuleHandleW(nullptr), nullptr);
            const std::array<const wchar_t*, 7> naturalnessOptions{
                L"选择自然度…", L"1：明显不自然", L"2：略不自然", L"3：自然度一般",
                L"4：较自然", L"5：非常自然", L"无法判断"};
            for (const auto* option : naturalnessOptions) SendMessageW(naturalness, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
            SendMessageW(naturalness, CB_SETMINVISIBLE, 7, 0);
            const HWND artifacts = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 0, 0, self->px(240), self->px(200), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(artifactRatingId)), GetModuleHandleW(nullptr), nullptr);
            const std::array<const wchar_t*, 8> artifactOptions{
                L"选择伪影情况…", L"无明显伪影", L"运动撕裂或错位", L"重影或拖影", L"闪烁或时间不稳",
                L"纹理或细节失真", L"遮挡边界错误", L"其他明显伪影"};
            for (const auto* option : artifactOptions) SendMessageW(artifacts, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
            SendMessageW(artifacts, CB_SETMINVISIBLE, 8, 0);
            control(L"BUTTON", L"记录评分", WS_TABSTOP | BS_OWNERDRAW, recordRatingId);
            control(L"BUTTON", L"← 上一帧", WS_TABSTOP | BS_OWNERDRAW, previousId);
            control(L"BUTTON", L"下一帧 →", WS_TABSTOP | BS_OWNERDRAW, nextId);
            control(L"BUTTON", L"播放 / 静音", WS_TABSTOP | BS_OWNERDRAW, playId);
            control(L"EDIT", L"0", WS_TABSTOP | ES_AUTOHSCROLL, timeId);
            control(L"BUTTON", L"定位 / 秒", WS_TABSTOP | BS_OWNERDRAW, goId);
            control(L"BUTTON", L"关闭", WS_TABSTOP | BS_OWNERDRAW, closeId);
            control(L"SCROLLBAR", L"比较位置", WS_TABSTOP | SBS_HORZ, sliderId);
            control(L"EDIT", L"选择原片和已完成结果，或导入本地质量评测报告。支持同步播放与精确逐帧。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, statusId);
            control(L"BUTTON", L"盲评需多候选", WS_TABSTOP | BS_OWNERDRAW, revealQualityCandidatesId);
            control(L"BUTTON", L"评审代码", WS_TABSTOP | BS_OWNERDRAW, reviewerCodeButtonId);
            control(L"BUTTON", L"导出评审", WS_TABSTOP | BS_OWNERDRAW, exportRatingsId);
            for (const int id : {candidateId, confidenceRatingId, naturalnessRatingId, artifactRatingId})
                SetWindowSubclass(GetDlgItem(window, id), comparisonComboProcedure, 1, reinterpret_cast<DWORD_PTR>(self));
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
            info->ptMinTrackSize.y = std::min<LONG>(self->px(self->reviewCandidates_.empty() ? 480 : 580), monitor.rcWork.bottom - monitor.rcWork.top); return 0;
        }
        case WM_COMMAND:
            if ((LOWORD(wParam) == naturalnessRatingId || LOWORD(wParam) == artifactRatingId || LOWORD(wParam) == confidenceRatingId) && HIWORD(wParam) == CBN_SELCHANGE) {
                self->updateReviewControls();
                return 0;
            }
            if (LOWORD(wParam) == candidateId && HIWORD(wParam) == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(GetDlgItem(window, candidateId), CB_GETCURSEL, 0, 0);
                if (index >= 0) self->activateCandidate(static_cast<std::size_t>(index));
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                case sourceBrowseId: self->choose(false); return 0;
                case outputBrowseId: self->choose(true); return 0;
                case reportBrowseId: self->chooseQualityReport(); return 0;
                case revealQualityCandidatesId: self->revealQualityCandidates(); return 0;
                case reviewerCodeButtonId: self->editReviewerCode(); return 0;
                case recordRatingId: self->recordCurrentRating(); return 0;
                case exportRatingsId: self->chooseRatingExport(); return 0;
                case previousReviewId: self->navigateReview(-1); return 0;
                case nextReviewId: self->navigateReview(1); return 0;
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
            if (!self->integrityChecking_ && reinterpret_cast<HWND>(lParam) == GetDlgItem(window, sliderId) && self->pair_ && self->pair_->duration) {
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
            if (wParam == integrityTimer) { self->updateIntegrityProgress(); return 0; }
            break;
        case loadedMessage: self->receive(); return 0;
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item && item->CtlType == ODT_COMBOBOX) { item->itemHeight = static_cast<UINT>(self->px(32)); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam); if (!item) return FALSE;
            const int saved = SaveDC(item->hDC);
            if (item->CtlType == ODT_COMBOBOX) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                HBRUSH brush = CreateSolidBrush(selected ? RGB(36, 66, 82) : RGB(14, 23, 34));
                FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
                std::wstring label;
                if (item->itemID != static_cast<UINT>(-1)) {
                    const auto length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
                    if (length > 0) {
                        label.resize(static_cast<std::size_t>(length) + 1);
                        SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(label.data()));
                        label.resize(static_cast<std::size_t>(length));
                    }
                }
                RECT text = item->rcItem; text.left += self->px(12); text.right -= self->px(12);
                SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, selected ? RGB(242, 247, 255) : RGB(220, 231, 242));
                if (self->font_) SelectObject(item->hDC, self->font_);
                DrawTextW(item->hDC, label.c_str(), static_cast<int>(label.size()), &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                if (item->itemState & ODS_FOCUS) { RECT focus = item->rcItem; InflateRect(&focus, -2, -2); DrawFocusRect(item->hDC, &focus); }
                RestoreDC(item->hDC, saved); return TRUE;
            }
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            HBRUSH brush = CreateSolidBrush(disabled ? RGB(29, 38, 49) : (item->itemState & ODS_SELECTED) ? RGB(37, 75, 66) : RGB(33, 52, 66));
            FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, disabled ? RGB(113, 130, 147) : RGB(223, 241, 238)); SelectObject(item->hDC, self->font_);
            wchar_t value[128]{}; GetWindowTextW(item->hwndItem, value, static_cast<int>(std::size(value))); RECT rect = item->rcItem;
            DrawTextW(item->hDC, value, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -self->px(3), -self->px(3)); DrawFocusRect(item->hDC, &rect); }
            RestoreDC(item->hDC, saved); return TRUE;
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(220, 233, 242)); SetBkColor(reinterpret_cast<HDC>(wParam), RGB(20, 25, 34)); return reinterpret_cast<LRESULT>(self->field_);
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); self->draw(dc); EndPaint(window, &paint); return 0;
        }
        case WM_PRINTCLIENT: self->draw(reinterpret_cast<HDC>(wParam)); return 0;
        case WM_CLOSE: self->close(); return 0;
        case WM_NCDESTROY:
            self->cancelPlayback(); self->invalidateRequests(); self->window_ = nullptr; self->pair_.reset(); self->busy_ = false;
            self->playbackMetrics_.reset(); self->playbackTelemetry_.clear();
            self->source_.clear(); self->output_.clear(); self->error_.clear(); self->pendingTime_ = Rational(0);
            self->qualityReportPath_.clear(); self->qualityReference_.clear(); self->qualityReferenceSha256_.clear();
            self->qualityReportFingerprint_.clear(); self->resumeSaveFailed_ = false;
            self->qualityReviewSessionId_.clear();
            self->blindRandomSeedWords_.reset();
            self->qualityReferenceIdentity_.reset(); self->qualityReferenceOrigin_.reset(); self->reviewCandidates_.clear(); self->mediaHashesVerified_ = false;
            self->blindReviewAvailable_ = false; self->blindReviewMode_ = false; self->blindReviewRevealed_ = false;
            self->integrityChecking_ = false; self->hashProgress_.reset(); self->selectedCandidate_ = 0; self->reviewCursor_.reset(); self->playing_ = false;
            KillTimer(window, seekTimer); KillTimer(window, playbackTimer); KillTimer(window, integrityTimer); SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::filesystem::path reviewDataRoot_;
    std::shared_ptr<Shared> shared_;
    HBRUSH background_ = nullptr, field_ = nullptr;
    HFONT font_ = nullptr;
    HWND window_ = nullptr;
    std::filesystem::path source_, output_;
    std::filesystem::path qualityReportPath_, qualityReference_;
    std::string qualityReferenceSha256_;
    std::string qualityReviewSessionId_, qualityReportFingerprint_;
    bool resumeSaveFailed_ = false;
    std::optional<std::array<std::uint32_t, 4>> blindRandomSeedWords_;
    std::optional<application::FileIdentity> qualityReferenceIdentity_;
    std::optional<Rational> qualityReferenceOrigin_;
    std::shared_ptr<NativeComparisonProgress> hashProgress_;
    std::vector<NativeQualityReviewCandidate> reviewCandidates_;
    std::size_t selectedCandidate_ = 0;
    std::optional<std::size_t> reviewCursor_;
    std::optional<NativeComparisonPair> pair_;
    std::shared_ptr<NativeComparisonPlaybackMetrics> playbackMetrics_;
    std::wstring error_;
    std::wstring reviewerCode_;
    std::wstring playbackTelemetry_;
    Rational pendingTime_;
    Rational playbackAnchor_;
    std::chrono::steady_clock::time_point playbackStarted_;
    std::chrono::steady_clock::time_point playbackStatsWindow_, lastPlaybackDetailsUpdate_, lastPlaybackSliderUpdate_;
    std::uint64_t lastSourceDecodedSample_ = 0, lastOutputDecodedSample_ = 0, lastSynchronizedSample_ = 0;
    std::uint64_t playbackDisplayedPairs_ = 0, lastDisplayedPairsSample_ = 0, playbackSkippedPairs_ = 0;
    bool playing_ = false, playbackClockReady_ = false;
    bool mediaHashesVerified_ = false, integrityChecking_ = false;
    bool blindReviewAvailable_ = false, blindReviewMode_ = false, blindReviewRevealed_ = false;
    bool busy_ = false;
    std::jthread worker_, playbackWorker_;
};

} // namespace rifelove::native_desktop

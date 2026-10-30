#pragma once

#include "native_comparison_window.hpp"
#include "native_model_center_test.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <fstream>
#include <memory>
#include <thread>

namespace rifelove::native_test {

// Tiny local MP4 fixture only: replace the first track-header display matrix
// without decoding/re-encoding, leaving the source and its packets untouched.
inline void writeComparisonMatrixFixture(const std::filesystem::path& source, const std::filesystem::path& destination,
    const std::array<std::int32_t, 9>& matrix) {
    const auto length = std::filesystem::file_size(source);
    if (length > 32 * 1024 * 1024 || length < 8) throw std::invalid_argument("Comparison matrix fixture needs a small MP4");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::ifstream input(source, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("Cannot read comparison matrix fixture");
    const auto u32 = [&](std::size_t offset) {
        if (offset + 4 > bytes.size()) throw std::runtime_error("Truncated MP4 fixture box");
        std::uint32_t value = 0; for (int index = 0; index < 4; ++index) value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)]; return value;
    };
    std::function<std::optional<std::size_t>(std::size_t, std::size_t, int)> locate;
    locate = [&](std::size_t begin, std::size_t end, int depth) -> std::optional<std::size_t> {
        if (depth > 4) throw std::runtime_error("Unexpected MP4 fixture nesting");
        for (auto offset = begin; offset + 8 <= end;) {
            std::uint64_t size = u32(offset); std::size_t header = 8;
            if (size == 1) { if (offset + 16 > end) throw std::runtime_error("Truncated extended fixture box"); size = (static_cast<std::uint64_t>(u32(offset + 8)) << 32) | u32(offset + 12); header = 16; }
            if (size == 0) size = end - offset;
            if (size < header || size > end - offset) throw std::runtime_error("Invalid MP4 fixture box size");
            const auto type = u32(offset + 4);
            const auto next = offset + static_cast<std::size_t>(size);
            if (type == 0x746b6864U) {
                const auto version = bytes[offset + header];
                if (version > 1) throw std::runtime_error("Unsupported MP4 fixture track header");
                const auto matrixOffset = offset + header + (version == 1 ? 52 : 40);
                if (matrixOffset + 36 > next) throw std::runtime_error("Missing MP4 fixture matrix");
                return matrixOffset;
            }
            if (type == 0x6d6f6f76U || type == 0x7472616bU)
                if (const auto found = locate(offset + header, next, depth + 1)) return found;
            offset = next;
        }
        return {};
    };
    const auto offset = locate(0, bytes.size(), 0);
    if (!offset) throw std::runtime_error("No track display matrix in MP4 comparison fixture");
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        const auto value = static_cast<std::uint32_t>(matrix[index]);
        for (int byte = 0; byte < 4; ++byte) bytes[*offset + index * 4 + static_cast<std::size_t>(byte)] =
            static_cast<std::uint8_t>(value >> (24 - byte * 8));
    }
    if (std::filesystem::exists(destination)) throw std::runtime_error("Comparison fixture never replaces an existing file");
    std::ofstream output(destination, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write comparison display-matrix fixture");
}

template<class Check, class Pump>
void checkNativeComparison(Check check, Pump pump, HWND owner) {
    using namespace native_desktop;
    using namespace std::chrono_literals;
    for (const auto& value : {L"0.5", L".5", L" 1/2 "})
        check(nativeComparisonTime(value) == Rational(1, 2), "comparison time accepts exact decimal and fraction seconds");
    check(nativeComparisonTime(L"1.000000001") == Rational(1000000001, 1000000000),
        "comparison decimal parsing preserves nanosecond precision without floating-point drift");
    for (const auto& value : {L"", L".", L"1.2.3", L"1.2345678910", L"1.5/2"}) {
        bool rejected = false; try { (void)nativeComparisonTime(value); } catch (const std::exception&) { rejected = true; }
        check(rejected, "comparison time rejects malformed input and excessive decimal precision");
    }
    media::DecodedPreviewFrame pixels;
    pixels.width = 1; pixels.height = 1; pixels.rgba = {12, 34, 56, 255};
    const auto converted = nativeComparisonImage(std::move(pixels));
    check(converted.bgra == std::vector<std::uint8_t>({56, 34, 12, 255}), "comparison display converts RGBA to native BGRA without swapping colors");
    media::MediaInfo metadata{}; metadata.preferredVideoStream = 0;
    media::StreamInfo stream{}; stream.index = 0; stream.type = "video"; stream.rotationDegrees = 0.0;
    const std::array<std::int32_t, 9> identity{65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824};
    stream.displayMatrix = identity; metadata.streams.push_back(stream);
    check(nativeComparisonStream(metadata).index == 0, "identity FFmpeg display matrix is accepted for coded-pixel comparison");
    for (const int element : {0, 4, 6, 7}) {
        auto transformed = identity;
        if (element == 0 || element == 4) transformed[static_cast<std::size_t>(element)] = -65536;
        else transformed[static_cast<std::size_t>(element)] = 65536;
        metadata.streams[0].displayMatrix = transformed;
        bool rejected = false; try { (void)nativeComparisonStream(metadata); } catch (const std::exception&) { rejected = true; }
        check(rejected, "zero rotation does not bypass rejection of reflected or translated display matrices");
    }
    metadata.streams[0].displayMatrix.reset(); metadata.streams[0].rotationDegrees = 90.0;
    bool rotationRejected = false; try { (void)nativeComparisonStream(metadata); } catch (const std::exception&) { rotationRejected = true; }
    check(rotationRejected, "explicit nonzero rotation is rejected even without an attached display matrix");
    stream.start.reset(); int discoveries = 0;
    check(nativeComparisonOrigin(stream, [&] { ++discoveries; return Rational(42, 5); }) == Rational(42, 5) && discoveries == 1,
        "absent stream start obtains the first verified decoded PTS instead of assuming zero");
    stream.start = Rational(-3, 2);
    check(nativeComparisonOrigin(stream, [&] { ++discoveries; return Rational(0); }) == Rational(-3, 2) && discoveries == 1,
        "declared negative stream origins remain exact and do not get normalized to zero");
    check(nativeComparisonAbsoluteTime(Rational(1, 90000), Rational(1, 2)) == Rational(45001, 90000),
        "comparison preserves an exactly representable absolute PTS without silently rounding its request");
    check(nativeComparisonAbsoluteTime(Rational(-3, 2), Rational(1, 2)) == Rational(-1),
        "comparison absolute-time validation preserves negative stream origins");
    for (const auto& request : {Rational(1, 1000000000), Rational(1, std::numeric_limits<std::int64_t>::max())}) {
        bool rejected = false;
        try { (void)nativeComparisonAbsoluteTime(Rational(1, 90000), request); }
        catch (const std::invalid_argument& error) {
            const auto message = nativeModelUtf8(std::wstring(L"起始 PTS"));
            rejected = std::string(error.what()).find(message) != std::string::npos &&
                std::string(error.what()).find(nativeModelUtf8(L"减少秒数小数位")) != std::string::npos;
        }
        check(rejected, "oversized-denominator or overflowing absolute PTS is explicitly refused with actionable precision guidance");
    }

    struct Counters { std::atomic<int> slowStarted{0}, cancelled{0}; };
    const auto counters = std::make_shared<Counters>();
    NativeComparisonWindow comparison([counters](const NativeComparisonRequest& request, std::stop_token stop) {
        if (request.relativeTime == Rational(1, 4)) {
            ++counters->slowStarted;
            for (int count = 0; count < 500; ++count) {
                if (stop.stop_requested()) { ++counters->cancelled; throw std::runtime_error("cancelled comparison fixture"); }
                std::this_thread::sleep_for(2ms);
            }
        }
        if (request.relativeTime == Rational(7, 8)) throw std::runtime_error("comparison fixture decode failure");
        NativeComparisonPair pair;
        pair.sourceOrigin = Rational(10); pair.outputOrigin = Rational(20); pair.duration = Rational(2);
        pair.requested = request.relativeTime;
        const auto image = [&](Rational origin, std::uint8_t red) {
            media::DecodedPreviewFrame frame; frame.width = 8; frame.height = 4;
            frame.timestamp = origin + request.relativeTime;
            if (request.relativeTime > Rational(0)) frame.previousTimestamp = frame.timestamp - Rational(1, 60);
            if (request.relativeTime < Rational(2)) frame.nextTimestamp = frame.timestamp + Rational(1, 60);
            for (int pixel = 0; pixel < 32; ++pixel) frame.rgba.insert(frame.rgba.end(), {red, 80, 140, 255});
            return nativeComparisonImage(std::move(frame));
        };
        pair.source = image(pair.sourceOrigin, 60); pair.output = image(pair.outputOrigin, 180);
        return pair;
    });
    const HWND window = comparison.open(owner, L"C:\\comparison-source.mp4", L"C:\\comparison-result.mkv");
    check(window != nullptr, "create an owned native comparison window");
    if (!window) return;
    check(pump([&] { return comparison.ready(); }, 5s), "comparison asynchronously displays its initial frame pair");
    if (!comparison.ready()) { comparison.close(); return; }
    check(comparison.pair()->source.timestamp == Rational(10) && comparison.pair()->output.timestamp == Rational(20),
        "comparison uses each stream's own origin for the same relative time");
    check(comparison.open(owner) == window, "opening comparison again reuses its existing window");
    comparison.step(1);
    check(pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(1, 60),
        "next-frame button follows the output's exact next presentation timestamp");
    comparison.step(-1);
    check(pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(0),
        "previous-frame button follows the exact previous presentation timestamp");

    SendMessageW(window, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 7500), reinterpret_cast<LPARAM>(GetDlgItem(window, 1309)));
    check(pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(3, 2),
        "dragging comparison location seeks both files to the same relative time");
    SetWindowTextW(GetDlgItem(window, 1307), L"0.5"); SendMessageW(GetDlgItem(window, 1308), BM_CLICK, 0, 0);
    check(pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(1, 2),
        "native time field and jump button accept decimal seconds");
    SetWindowTextW(GetDlgItem(window, 1307), L"invalid"); SendMessageW(GetDlgItem(window, 1308), BM_CLICK, 0, 0);
    check(comparison.ready() && comparison.pair()->requested == Rational(1, 2), "invalid time input preserves the displayed valid pair");

    comparison.seek(Rational(1, 4));
    check(pump([&] { return counters->slowStarted.load() > 0; }, 5s), "slow comparison request starts on its worker");
    comparison.seek(Rational(3, 4));
    check(pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(3, 4) && counters->cancelled.load() > 0,
        "a newer seek cancels and supersedes the old decode without stale frame publication");
    comparison.seek(Rational(7, 8));
    check(pump([&] { return !comparison.busy(); }, 5s) && !comparison.ready() && !comparison.pair() && !comparison.error().empty(),
        "decode failure clears stale comparison pixels and reports an error");
    comparison.seek(Rational(1)); check(pump([&] { return comparison.ready(); }, 5s), "comparison can recover by seeking after an error");

    RECT bounds{}; GetWindowRect(window, &bounds);
    const UINT dpi = GetDpiForWindow(window);
    const auto px = [&](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96U), 96); };
    for (const int width : {480, 640, 1000}) {
        SetWindowPos(window, nullptr, bounds.left, bounds.top, px(width), px(520), SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{}; GetClientRect(window, &client);
        std::vector<RECT> actions;
        for (const int id : {1303, 1304, 1305, 1306, 1307, 1308, 1309, 1310, 1311}) {
            RECT field{}; GetWindowRect(GetDlgItem(window, id), &field);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&field), 2);
            check(IsWindowVisible(GetDlgItem(window, id)) && field.left >= 0 && field.top >= 0 && field.right <= client.right && field.bottom <= client.bottom,
                "comparison controls remain visible inside resized native windows");
            if (id == 1305 || id == 1306 || id == 1307 || id == 1308 || id == 1311) actions.push_back(field);
        }
        for (std::size_t first = 0; first < actions.size(); ++first)
            for (std::size_t second = first + 1; second < actions.size(); ++second) {
                RECT overlap{}; check(!IntersectRect(&overlap, &actions[first], &actions[second]), "comparison transport controls retain distinct hit targets on narrow windows");
            }
    }
    comparison.seek(Rational(1, 4));
    check(pump([&] { return counters->slowStarted.load() > 1; }, 5s), "second slow comparison request starts");
    const auto beforeClose = std::chrono::steady_clock::now(); comparison.close();
    check(std::chrono::steady_clock::now() - beforeClose < 250ms && !IsWindow(window), "closing comparison cancels pending work without joining it on the UI thread");
    const HWND reopened = comparison.open(owner, L"C:\\comparison-source.mp4", L"C:\\comparison-result.mkv");
    check(reopened && pump([&] { return comparison.ready(); }, 5s) && comparison.pair()->requested == Rational(0),
        "comparison safely reopens without receiving the old window's stale result");
    MSG escape{}; escape.hwnd = GetDlgItem(reopened, 1307); escape.message = WM_KEYDOWN; escape.wParam = VK_ESCAPE;
    check(comparison.handleDialogMessage(escape) && !comparison.window(), "Escape safely closes the modeless comparison window");
    const HWND emptyReopen = comparison.open(owner);
    wchar_t sourcePath[128]{}, outputPath[128]{};
    GetWindowTextW(GetDlgItem(emptyReopen, 1301), sourcePath, static_cast<int>(std::size(sourcePath)));
    GetWindowTextW(GetDlgItem(emptyReopen, 1302), outputPath, static_cast<int>(std::size(outputPath)));
    check(emptyReopen && !sourcePath[0] && !outputPath[0] && !IsWindowEnabled(GetDlgItem(emptyReopen, 1308)) &&
        !comparison.busy() && !comparison.pair(),
        "reopening without main-workspace paths clears both visible and internal files and disables hidden old-file seeks");
    comparison.seek(Rational(1));
    check(!comparison.busy() && !comparison.pair(), "empty reopened comparison cannot decode files retained by an earlier window");
    comparison.close();
    const HWND externallyClosed = comparison.open(owner, L"C:\\comparison-source.mp4", L"C:\\comparison-result.mkv");
    check(externallyClosed && pump([&] { return comparison.ready(); }, 5s), "comparison opens again with explicit visible files");
    DestroyWindow(externallyClosed);
    const HWND afterExternalClose = comparison.open(owner);
    check(afterExternalClose && !IsWindowEnabled(GetDlgItem(afterExternalClose, 1308)) && !comparison.busy(),
        "owner-driven destruction also clears internal paths before an empty reopen");
    comparison.close();
}

template<class Check, class Pump, class Capture>
void checkNativeComparisonFiles(Check check, Pump pump, Capture capture, HWND owner,
    const std::filesystem::path& source, const std::filesystem::path& output, const std::filesystem::path& screenshot = {}) {
    using namespace native_desktop;
    using namespace std::chrono_literals;
    ModelCenterTemporaryDirectory matrixFixtures;
    for (const int element : {4, 6}) {
        std::array<std::int32_t, 9> matrix{65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824};
        matrix[static_cast<std::size_t>(element)] = element == 4 ? -65536 : 65536;
        const auto fixture = matrixFixtures.root / (element == 4 ? L"vertical-reflection.mp4" : L"display-offset.mp4");
        writeComparisonMatrixFixture(source, fixture, matrix);
        const auto info = media::MediaEngine::probe(nativeModelUtf8(fixture.wstring()));
        const auto video = std::find_if(info.streams.begin(), info.streams.end(), [&](const auto& stream) { return stream.index == info.preferredVideoStream; });
        check(video != info.streams.end() && video->displayMatrix == matrix && video->rotationDegrees && *video->rotationDegrees == 0.0,
            "real MP4 probe preserves reflection/offset matrix while reporting zero rotation");
        bool rejected = false;
        try { (void)NativeComparisonWindow::loadPair({fixture, output, Rational(0), {}, 1}, {}); }
        catch (const std::exception& error) { rejected = std::string(error.what()).find("display-matrix") != std::string::npos; }
        check(rejected, "real zero-angle reflected/offset video is rejected by comparison before uncorrected pixels are displayed");
    }
    NativeComparisonWindow comparison;
    const HWND window = comparison.open(owner, source, output);
    check(window != nullptr, "create real-media native comparison window");
    if (!window) return;
    const auto settled = [&] { return pump([&] { return !comparison.busy(); }, 30s) && comparison.ready(); };
    check(settled(), "real local source and completed output decode into native comparison without an inference model");
    if (!comparison.ready()) {
        std::cerr << "Comparison decode error: " << nativeModelUtf8(comparison.error()) << '\n'; comparison.close(); return;
    }
    const auto aligned = [&](Rational request) {
        const auto sourceDelta = comparison.pair()->source.timestamp - comparison.pair()->sourceOrigin - request;
        const auto outputDelta = comparison.pair()->output.timestamp - comparison.pair()->outputOrigin - request;
        return std::abs(sourceDelta.toLongDouble()) <= 0.05L && std::abs(outputDelta.toLongDouble()) <= 0.05L;
    };
    check(aligned(Rational(0)), "real comparison starts at the same relative presentation time");
    const auto sampleTime = comparison.pair()->duration ? std::min(Rational(1, 2), *comparison.pair()->duration / Rational(3)) : Rational(1, 2);
    comparison.seek(sampleTime); check(settled() && aligned(sampleTime), "real comparison seeks both sources near the same interior timestamp");
    if (!comparison.ready()) { comparison.close(); return; }
    const auto next = comparison.pair()->output.next;
    check(next.has_value(), "real comparison exposes its next output presentation timestamp");
    if (next) {
        comparison.step(1); check(settled() && comparison.pair()->output.timestamp == *next, "real next-frame action selects exactly the adjacent output frame");
        if (!comparison.ready()) { comparison.close(); return; }
        const auto previous = comparison.pair()->output.previous;
        if (previous) {
            comparison.step(-1); check(settled() && comparison.pair()->output.timestamp == *previous, "real previous-frame action returns to the exact earlier output frame");
        }
    }
    if (!comparison.ready()) { comparison.close(); return; }
    check(comparison.pair()->duration.has_value(), "real comparison derives a common video duration for its location slider");
    if (comparison.pair()->duration) {
        const auto duration = *comparison.pair()->duration;
        SendMessageW(window, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 7500), reinterpret_cast<LPARAM>(GetDlgItem(window, 1309)));
        check(settled() && aligned(duration * Rational(3, 4)), "real slider locates the same timestamp in the original and completed result");
        comparison.seek(duration); check(settled(), "real end-of-video seek displays the last available presentation frame");
        if (comparison.ready()) {
            check(!comparison.pair()->output.next && !IsWindowEnabled(GetDlgItem(window, 1306)), "last comparison frame disables unavailable next-frame navigation");
            comparison.step(-1); check(settled(), "real last-frame comparison can step backward without requesting a nonexistent next frame");
        }
    }
    comparison.seek(Rational(0)); check(settled(), "real comparison returns to the beginning after end-of-file navigation");
    if (!screenshot.empty()) {
        check(capture(window, screenshot), "capture the real native comparison with decoded video frames");
        const UINT dpi = GetDpiForWindow(window); RECT bounds{}; GetWindowRect(window, &bounds);
        SetWindowPos(window, nullptr, bounds.left, bounds.top, MulDiv(640, static_cast<int>(dpi), 96), MulDiv(520, static_cast<int>(dpi), 96), SWP_NOZORDER | SWP_NOACTIVATE);
        auto narrow = screenshot; narrow.replace_filename(narrow.stem().wstring() + L"-narrow" + narrow.extension().wstring());
        check(capture(window, narrow), "capture the resized native comparison with the same decoded frame pair");
    }
    comparison.close();
}

} // namespace rifelove::native_test

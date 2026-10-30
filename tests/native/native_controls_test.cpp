#include "native_app.hpp"
#include "native_renderer.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

using rifelove::native_desktop::NativeApp;
using rifelove::native_desktop::NativeLayout;

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

BOOL CALLBACK findWindow(HWND window, LPARAM result) {
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (std::wstring(name) == L"RifeLoveNativeDesktopWindow") {
        *reinterpret_cast<HWND*>(result) = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK findRejectionWindow(HWND window, LPARAM result) {
    wchar_t title[64]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring(title) == L"被拒绝的素材") {
        *reinterpret_cast<HWND*>(result) = window;
        return FALSE;
    }
    return TRUE;
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
BOOL CALLBACK findHistoryWindow(HWND window, LPARAM result) {
    wchar_t title[128]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (std::wstring(title) == L"RifeLove · 任务历史") {
        *reinterpret_cast<HWND*>(result) = window;
        return FALSE;
    }
    return TRUE;
}
#endif

bool writeBitmap(const std::filesystem::path& path, const BITMAPINFOHEADER& info, const void* pixels) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels), info.biSizeImage);
    return static_cast<bool>(stream);
}

void checkControlRendering(HWND parent, HWND combo, HWND sourceButton, HWND startButton,
                           const std::filesystem::path& output) {
    constexpr int width = 420, height = 190;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = width * height * 4;
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap || !pixels) {
        check(false, "create off-screen control bitmap");
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return;
    }
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT whole{0, 0, width, height};
    HBRUSH canvas = CreateSolidBrush(RGB(19, 24, 32));
    FillRect(dc, &whole, canvas);
    DeleteObject(canvas);

    SetViewportOrgEx(dc, 16, 12, nullptr);
    SendMessageW(combo, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    SetViewportOrgEx(dc, 16, 56, nullptr);
    DRAWITEMSTRUCT button{};
    button.CtlType = ODT_BUTTON;
    button.CtlID = 1002;
    button.hwndItem = sourceButton;
    button.hDC = dc;
    button.rcItem = RECT{0, 0, 300, 34};
    SendMessageW(parent, WM_DRAWITEM, button.CtlID, reinterpret_cast<LPARAM>(&button));
    SetViewportOrgEx(dc, 16, 100, nullptr);
    button.CtlID = 1009;
    button.hwndItem = startButton;
    button.itemState = ODS_DISABLED;
    SendMessageW(parent, WM_DRAWITEM, button.CtlID, reinterpret_cast<LPARAM>(&button));
    SetViewportOrgEx(dc, 16, 144, nullptr);
    DRAWITEMSTRUCT option{};
    option.CtlType = ODT_COMBOBOX;
    option.CtlID = 1005;
    option.itemID = 1;
    option.itemState = ODS_SELECTED;
    option.hwndItem = combo;
    option.hDC = dc;
    option.rcItem = RECT{0, 0, 300, 32};
    SendMessageW(parent, WM_DRAWITEM, option.CtlID, reinterpret_cast<LPARAM>(&option));
    SetViewportOrgEx(dc, 0, 0, nullptr);

    check(GetPixel(dc, 30, 66) == RGB(145, 224, 192), "enabled button uses accent fill");
    check(GetPixel(dc, 30, 110) == RGB(38, 49, 63), "disabled button uses disabled fill");
    check(GetPixel(dc, 30, 154) == RGB(36, 66, 82), "selected drop-down item uses selected fill");
    if (!output.empty()) check(writeBitmap(output, info.bmiHeader, pixels), "write off-screen visual evidence");
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void checkPathFields(HWND window, bool clearAfter = true) {
    const HWND source = GetDlgItem(window, 1001);
    const HWND output = GetDlgItem(window, 1003);
    check(source && output, "find full-path edit controls");
    if (!source || !output) return;
    const std::wstring sourcePath = L"C:\\Users\\Administrator\\Documents\\RifeLove\\source\\a-very-long-source-video-name-for-path-accessibility-test.mp4";
    const std::wstring outputPath = L"C:\\Users\\Administrator\\Documents\\RifeLove\\output\\a-very-long-output-video-name-for-path-accessibility-test.mkv";
    SetWindowTextW(source, sourcePath.c_str());
    SetWindowTextW(output, outputPath.c_str());
    check(GetWindowTextLengthW(source) == static_cast<int>(sourcePath.size()),
          "source path edit keeps the complete long path");
    check(GetWindowTextLengthW(output) == static_cast<int>(outputPath.size()),
          "output path edit keeps the complete long path");
    const auto sourceStyle = GetWindowLongPtrW(source, GWL_STYLE);
    const auto outputStyle = GetWindowLongPtrW(output, GWL_STYLE);
    check((sourceStyle & ES_AUTOHSCROLL) != 0 && (sourceStyle & ES_MULTILINE) == 0,
          "source path is a single-line horizontal-scrolling field");
    check((outputStyle & ES_AUTOHSCROLL) != 0 && (outputStyle & ES_MULTILINE) == 0,
          "output path is a single-line horizontal-scrolling field");
    DWORD sourceSelectionStart = 0, sourceSelectionEnd = 0;
    DWORD outputSelectionStart = 0, outputSelectionEnd = 0;
    SendMessageW(source, EM_GETSEL, reinterpret_cast<WPARAM>(&sourceSelectionStart),
                 reinterpret_cast<LPARAM>(&sourceSelectionEnd));
    SendMessageW(output, EM_GETSEL, reinterpret_cast<WPARAM>(&outputSelectionStart),
                 reinterpret_cast<LPARAM>(&outputSelectionEnd));
    check(sourceSelectionStart == sourcePath.size() && sourceSelectionEnd == sourcePath.size(),
          "source path opens scrolled to its filename while preserving the full value");
    check(outputSelectionStart == outputPath.size() && outputSelectionEnd == outputPath.size(),
          "output path opens scrolled to its filename while preserving the full value");
    check(SendMessageW(source, EM_GETLINECOUNT, 0, 0) >= 1,
          "source path remains keyboard accessible when horizontally scrolled");
    check(SendMessageW(output, EM_GETLINECOUNT, 0, 0) >= 1,
          "output path remains keyboard accessible when horizontally scrolled");
    if (clearAfter) {
        SetWindowTextW(source, L"");
        SetWindowTextW(output, L"");
    }
}

bool captureClient(HWND window, const std::filesystem::path& output) {
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const int width = client.right, height = client.bottom;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(width * height * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    // Print the direct children using their actual window procedures. This
    // builds a composite without reading pixels from the user's desktop.
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT position{};
        GetWindowRect(child, &position);
        POINT origin{position.left, position.top};
        ScreenToClient(window, &origin);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN);
        RestoreDC(dc, saved);
    }
    const bool written = writeBitmap(output, info.bmiHeader, pixels);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return written;
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "(conversion failed)";
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

bool pumpUntil(const std::function<bool()>& condition, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (condition()) return true;
        Sleep(20);
    }
    return false;
}

void checkTaskHistory(NativeApp& app, HWND window) {
    const HWND button = GetDlgItem(window, 1020);
    check(button && IsWindowVisible(button), "task history button is visible");
    if (!button) return;
    SendMessageW(button, BM_CLICK, 0, 0);
    HWND history = nullptr;
    check(pumpUntil([&] {
        EnumThreadWindows(GetCurrentThreadId(), findHistoryWindow, reinterpret_cast<LPARAM>(&history));
        return history != nullptr;
    }, std::chrono::seconds(3)), "task history window opens");
    if (!history) return;
    const HWND text = FindWindowExW(history, nullptr, L"EDIT", nullptr);
    check(text != nullptr, "task history has a scrollable read-only text view");
    check(pumpUntil([&] {
        HWND status = FindWindowExW(history, nullptr, L"STATIC", nullptr);
        wchar_t value[128]{};
        if (status) GetWindowTextW(status, value, static_cast<int>(std::size(value)));
        return status && std::wstring(value).find(L"正在读取") == std::wstring::npos;
    }, std::chrono::seconds(3)), "task history finishes loading asynchronously");
    if (text) {
        const int length = GetWindowTextLengthW(text);
        std::wstring contents(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
        GetWindowTextW(text, contents.data(), length + 1);
        check(contents.find(L"任务") != std::wstring::npos || contents.find(L"暂无本地任务记录") != std::wstring::npos,
            "task history shows an empty state or task records");
    }
    DestroyWindow(history);
    (void)app;
}

std::wstring comboItem(HWND combo, int index) {
    if (!combo || index < 0) return {};
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));
    return value;
}

void checkRestorationControls(NativeApp& app, HWND window, bool requireDenoise) {
    const HWND temporal = GetDlgItem(window, 1024);
    const HWND restoration = GetDlgItem(window, 1017);
    const HWND outputSize = GetDlgItem(window, 1018);
    check(temporal && restoration && outputSize, "find independent restoration controls");
    if (!temporal || !restoration || !outputSize) return;

    check(SendMessageW(temporal, CB_GETCURSEL, 0, 0) == 0, "optional denoise defaults to off");
    check(SendMessageW(restoration, CB_GETCURSEL, 0, 0) == 0, "optional restoration defaults to off");
    const auto temporalOffLabel = comboItem(temporal, 0);
    const auto spatialOffLabel = comboItem(restoration, 0);
    check(temporalOffLabel.find(L"默认") != std::wstring::npos && temporalOffLabel.find(L"降噪") != std::wstring::npos,
        "optional denoise off label explains the default");
    check(spatialOffLabel.find(L"默认") != std::wstring::npos && spatialOffLabel.find(L"超分") != std::wstring::npos,
        "spatial restoration off label explains the default");

    int denoiseIndex = -1;
    const int count = static_cast<int>(SendMessageW(temporal, CB_GETCOUNT, 0, 0));
    for (int index = 1; index < count; ++index) {
        const auto label = comboItem(temporal, index);
        if (label.find(L"FastDVDnet") != std::wstring::npos && label.find(L"降噪") != std::wstring::npos) {
            denoiseIndex = index;
            break;
        }
    }
    if (requireDenoise) {
        check(denoiseIndex >= 0, "FastDVDnet is discoverable as optional denoise");
        check(app.testViewState().modelSummary.find(L"默认包") != std::wstring::npos,
              "the bundled default model marker is honored");
    }
    if (denoiseIndex >= 0) {
        SendMessageW(temporal, CB_SETCURSEL, denoiseIndex, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporal));
        check(IsWindowEnabled(outputSize) == FALSE, "FastDVDnet disables fixed output size");
        check(app.testViewState().restorationSummary.find(L"降噪") != std::wstring::npos &&
              app.testViewState().restorationSummary.find(L"超分：关闭") != std::wstring::npos,
            "selected FastDVDnet summary identifies denoise");
    }

    int spatialIndex = -1;
    for (int index = 1; index < static_cast<int>(SendMessageW(restoration, CB_GETCOUNT, 0, 0)); ++index) {
        if (comboItem(restoration, index).find(L"超分辨率") != std::wstring::npos) {
            spatialIndex = index;
            break;
        }
    }
    if (spatialIndex >= 0) {
        SendMessageW(restoration, CB_SETCURSEL, spatialIndex, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
        check(app.testViewState().restorationSummary.find(L"超分") != std::wstring::npos &&
              app.testViewState().restorationSummary.find(L"降噪") != std::wstring::npos,
            "spatial restoration can be selected independently");
        if (denoiseIndex >= 0)
            check(app.testViewState().restorationSummary.find(L"FastDVDnet") != std::wstring::npos,
                "denoise and spatial model selections coexist");
    }

    SendMessageW(temporal, CB_SETCURSEL, 0, 0);
    SendMessageW(restoration, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporal));
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    check(SendMessageW(outputSize, CB_GETCURSEL, 0, 0) == 0, "turning optional restoration off resets output size");
    check(IsWindowEnabled(outputSize) == FALSE, "turning optional restoration off disables output size");
}

void checkRapidSourceSwitch(NativeApp& app, HWND window,
                            const std::filesystem::path& first,
                            const std::filesystem::path& second) {
    if (!std::filesystem::is_regular_file(first) || !std::filesystem::is_regular_file(second)) {
        check(false, "rapid source switch fixtures must exist");
        return;
    }
    const HWND source = GetDlgItem(window, 1001);
    check(source != nullptr, "find source edit for rapid source switch");
    if (!source) return;
    SetWindowTextW(source, first.c_str());
    SetWindowTextW(source, second.c_str());
    const bool settled = pumpUntil([&] {
        const auto& state = app.testViewState();
        return state.sourceReady && state.mediaSummary.find(L"960/17") != std::wstring::npos;
    }, std::chrono::seconds(30));
    check(settled, "rapid source switch settles on the latest source inspection");
    if (!settled) {
        std::cerr << "Rapid switch status: " << utf8(app.testViewState().status)
                  << "; media: " << utf8(app.testViewState().mediaSummary) << '\n';
    }
    check(app.testViewState().mediaSummary.find(L"720/17") == std::wstring::npos,
        "stale inspection result does not overwrite the latest source");
}

void checkDenoiseColorRejection(NativeApp& app, HWND window, const std::filesystem::path& source,
                                const std::filesystem::path& output) {
    const HWND restoration = GetDlgItem(window, 1024), sourceEdit = GetDlgItem(window, 1001);
    const HWND outputEdit = GetDlgItem(window, 1003), start = GetDlgItem(window, 1009);
    if (!restoration || !sourceEdit || !outputEdit || !start || !std::filesystem::is_regular_file(source)) {
        check(false, "color-rejection fixture and controls must exist"); return;
    }
    int denoise = -1;
    for (int index = 1; index < static_cast<int>(SendMessageW(restoration, CB_GETCOUNT, 0, 0)); ++index)
        if (comboItem(restoration, index).find(L"FastDVDnet") != std::wstring::npos) { denoise = index; break; }
    if (denoise < 0) { check(false, "find temporal denoise for color-rejection test"); return; }
    SendMessageW(restoration, CB_SETCURSEL, denoise, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    SetWindowTextW(sourceEdit, source.c_str());
    SetWindowTextW(outputEdit, output.c_str());
    const bool inspected = pumpUntil([&] { return app.testViewState().sourceReady; }, std::chrono::seconds(30));
    check(inspected, "inspect the SDR color-rejection fixture");
    if (inspected) {
        check(app.testViewState().status.find(L"所选可选降噪模型不支持该视频的色彩配置") != std::wstring::npos,
              "selected denoiser reports its incompatible input color");
        check(IsWindowEnabled(start) == FALSE, "incompatible denoiser cannot start a task");
    }
}

void checkPauseResume(NativeApp& app, HWND window, const std::filesystem::path& source,
                      const std::filesystem::path& output) {
    if (!std::filesystem::is_regular_file(source) || std::filesystem::exists(output)) {
        check(false, "pause/resume fixture must exist and output must be new");
        return;
    }
    const HWND sourceEdit = GetDlgItem(window, 1001), outputEdit = GetDlgItem(window, 1003);
    const HWND rate = GetDlgItem(window, 1005), encoder = GetDlgItem(window, 1006);
    const HWND temporal = GetDlgItem(window, 1024), restoration = GetDlgItem(window, 1017), outputSize = GetDlgItem(window, 1018);
    const HWND start = GetDlgItem(window, 1009), pause = GetDlgItem(window, 1023), cancel = GetDlgItem(window, 1010);
    if (!sourceEdit || !outputEdit || !rate || !encoder || !temporal || !restoration || !outputSize || !start || !pause || !cancel) {
        check(false, "find pause/resume controls");
        return;
    }
    SendMessageW(temporal, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporal));
    SendMessageW(restoration, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    SendMessageW(outputSize, CB_SETCURSEL, 0, 0);
    SendMessageW(rate, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
    SendMessageW(encoder, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1006, CBN_SELCHANGE), reinterpret_cast<LPARAM>(encoder));
    SetWindowTextW(sourceEdit, source.c_str());
    SetWindowTextW(outputEdit, output.c_str());
    check(pumpUntil([&] { return IsWindowEnabled(start) != FALSE; }, std::chrono::seconds(30)),
        "pause/resume task becomes ready");
    if (!IsWindowEnabled(start)) return;

    SendMessageW(start, BM_CLICK, 0, 0);
    const bool started = pumpUntil([&] { return IsWindowEnabled(pause) != FALSE && app.testViewState().outputFrames > 0; }, std::chrono::seconds(90));
    check(started, "pause/resume task reaches a running checkpoint");
    if (!started) return;

    SendMessageW(pause, BM_CLICK, 0, 0);
    const bool paused = pumpUntil([&] {
        wchar_t label[32]{};
        GetWindowTextW(pause, label, static_cast<int>(std::size(label)));
        return app.testViewState().stage == L"已暂停" && std::wstring(label) == L"继续";
    }, std::chrono::seconds(90));
    check(paused, "pause button reaches the real paused queue state");
    if (!paused) return;
    check(!std::filesystem::exists(output), "paused task does not publish an incomplete output");
    check(app.testViewState().status.find(L"断点恢复") != std::wstring::npos,
        "paused status distinguishes session pause from checkpoint recovery");

    SendMessageW(pause, BM_CLICK, 0, 0);
    const bool completed = pumpUntil([&] {
        return std::filesystem::is_regular_file(output) && app.testViewState().stage == L"completed" && IsWindowEnabled(cancel) == FALSE;
    }, std::chrono::seconds(180));
    check(completed, "continued task publishes a completed output");
}

void checkProcessing(NativeApp& app, HWND window, const std::filesystem::path& source,
                     const std::filesystem::path& output, const std::filesystem::path& screenshot, bool cancelRun = false,
                     bool upscaleYoutube = false, bool temporalDenoise = false, bool dual = false,
                     bool motionBlur = false) {
    if (!std::filesystem::is_regular_file(source) || std::filesystem::exists(output)) {
        check(false, "processing fixture must exist and output must be new");
        return;
    }
    const HWND sourceEdit = GetDlgItem(window, 1001), outputEdit = GetDlgItem(window, 1003);
    const HWND rate = GetDlgItem(window, 1005), encoder = GetDlgItem(window, 1006), platform = GetDlgItem(window, 1012);
    const HWND temporal = GetDlgItem(window, 1024), restoration = GetDlgItem(window, 1017), outputSize = GetDlgItem(window, 1018);
    const HWND start = GetDlgItem(window, 1009), cancel = GetDlgItem(window, 1010);
    const HWND blur = GetDlgItem(window, 1013);
    if (!sourceEdit || !outputEdit || !rate || !encoder || !platform || !temporal || !restoration || !outputSize || !start || !cancel) {
        check(false, "find processing controls");
        return;
    }
    SendMessageW(platform, CB_SETCURSEL, upscaleYoutube ? 1 : 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), reinterpret_cast<LPARAM>(platform));
    // The earlier control test intentionally selects the last entry in each
    // list. Restore the ordinary independent selections for this fixture.
    int selectedTemporal = 0;
    int selectedSpatial = 0;
    if (temporalDenoise || dual) {
        selectedTemporal = -1;
        for (int index = 1; index < static_cast<int>(SendMessageW(temporal, CB_GETCOUNT, 0, 0)); ++index) {
            if (comboItem(temporal, index).find(L"FastDVDnet") != std::wstring::npos) {
                selectedTemporal = index;
                break;
            }
        }
    }
    if (upscaleYoutube || dual) {
        selectedSpatial = -1;
        for (int index = 1; index < static_cast<int>(SendMessageW(restoration, CB_GETCOUNT, 0, 0)); ++index) {
            if (comboItem(restoration, index).find(L"超分辨率") != std::wstring::npos) {
                selectedSpatial = index;
                break;
            }
        }
    }
    check(selectedTemporal >= 0 && selectedSpatial >= 0, "find requested independent restoration models");
    if (selectedTemporal < 0 || selectedSpatial < 0) return;
    check(SendMessageW(temporal, CB_SETCURSEL, selectedTemporal, 0) != CB_ERR, "select temporal denoise model");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporal));
    check(SendMessageW(restoration, CB_SETCURSEL, selectedSpatial, 0) != CB_ERR, "select spatial restoration model");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    SendMessageW(outputSize, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    SendMessageW(rate, CB_SETCURSEL, 1, 0); // 60 FPS.
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
    SendMessageW(encoder, CB_SETCURSEL, 1, 0); // Explicit H.264 NVENC on the test GPU.
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1006, CBN_SELCHANGE), reinterpret_cast<LPARAM>(encoder));
    SetWindowTextW(sourceEdit, source.c_str());
    SetWindowTextW(outputEdit, output.c_str());
    if (!pumpUntil([&] { return IsWindowEnabled(start) != FALSE; }, std::chrono::seconds(30))) {
        const auto& state = app.testViewState();
        std::cerr << "UI status: " << utf8(state.status) << "; model: " << utf8(state.modelSummary)
                  << "; runtime: " << utf8(state.runtimeSummary) << "; media: " << utf8(state.mediaSummary)
                  << "; modelReady=" << state.modelReady << "; runtimeReady=" << state.runtimeReady
                  << "; sourceReady=" << state.sourceReady << '\n';
        check(false, "native UI did not become ready after inspecting the test video");
        return;
    }
    if (motionBlur) {
        check(blur != nullptr, "find motion blur control for dual-blur processing");
        if (!blur) return;
        const bool available = pumpUntil([&] { return IsWindowEnabled(blur) != FALSE; }, std::chrono::seconds(5));
        check(available, "motion blur control becomes available for dual-blur processing");
        if (!available) return;
        SendMessageW(blur, BM_CLICK, 0, 0);
        check(SendMessageW(blur, BM_GETCHECK, 0, 0) == BST_CHECKED,
              "dual-blur processing enables the motion blur control");
        check(IsWindowEnabled(start) != FALSE, "dual-blur processing remains startable after enabling motion blur");
    }
    if (!screenshot.empty()) check(captureClient(window, screenshot), "capture imported processing UI");
    SendMessageW(start, BM_CLICK, 0, 0);
    if (cancelRun) {
        const bool started = pumpUntil([&] { return app.testViewState().outputFrames > 0; }, std::chrono::seconds(60));
        check(started, "processing produces a frame before cancellation");
        if (!started) return;
        SendMessageW(cancel, BM_CLICK, 0, 0);
        const bool stopped = pumpUntil([&] { return !IsWindowEnabled(cancel); }, std::chrono::seconds(60));
        check(stopped, "cancel returns the window to idle");
        check(!std::filesystem::exists(output), "cancel does not publish an incomplete output");
        if (stopped) check(app.testViewState().stage == L"cancelled", "cancel reports cancelled state");
        return;
    }
    const bool published = pumpUntil([&] {
        return std::filesystem::is_regular_file(output) && IsWindowEnabled(cancel) == FALSE;
    }, std::chrono::seconds(120));
    const auto& finalState = app.testViewState();
    if (!published) std::cerr << "UI status: " << utf8(finalState.status) << '\n';
    check(published, "native start button publishes a completed output");
    if (published) {
        check(finalState.stage == L"completed", "native UI reports completed state");
        std::cout << "Native processing: stage=" << utf8(finalState.stage)
                  << "; encoder=" << utf8(finalState.encoderSummary)
                  << "; frames=" << finalState.outputFrames << '\n';
    }
}

void checkBatchProcessing(NativeApp& app, HWND window, const std::filesystem::path& directory, bool expectRejection, bool cancelBatch, bool allRejected, bool youtube) {
    const auto first = directory / (youtube ? L"first_RifeLove.mp4" : L"first_RifeLove.mkv");
    const auto second = directory / (youtube ? L"second_RifeLove.mp4" : L"second_RifeLove.mkv");
    if (!std::filesystem::is_directory(directory) || std::filesystem::exists(first) || std::filesystem::exists(second)) {
        check(false, "batch fixture directory and new outputs required");
        return;
    }
    const HWND rate = GetDlgItem(window, 1005), encoder = GetDlgItem(window, 1006);
    const HWND platform = GetDlgItem(window, 1012), temporal = GetDlgItem(window, 1024), restoration = GetDlgItem(window, 1017);
    const HWND size = GetDlgItem(window, 1018), start = GetDlgItem(window, 1009), cancel = GetDlgItem(window, 1010);
    SendMessageW(platform, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), reinterpret_cast<LPARAM>(platform));
    SendMessageW(temporal, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporal));
    SendMessageW(restoration, CB_SETCURSEL, 0, 0);
    SendMessageW(size, CB_SETCURSEL, 0, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1017, CBN_SELCHANGE), reinterpret_cast<LPARAM>(restoration));
    SendMessageW(rate, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
    SendMessageW(encoder, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1006, CBN_SELCHANGE), reinterpret_cast<LPARAM>(encoder));
    const auto text = directory.wstring();
    const auto bytes = sizeof(DROPFILES) + (text.size() + 2) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) { check(false, "allocate batch drop"); return; }
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!drop) { GlobalFree(memory); check(false, "lock batch drop"); return; }
    drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
    std::copy(text.begin(), text.end(), reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES)));
    GlobalUnlock(memory);
    SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
    if (youtube) {
        SendMessageW(platform, CB_SETCURSEL, 1, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), reinterpret_cast<LPARAM>(platform));
    }
    if (!pumpUntil([&] { return IsWindowEnabled(start) != FALSE; }, std::chrono::seconds(30))) {
        std::cerr << "Batch readiness: " << utf8(app.testViewState().status) << '\n';
        check(false, "batch start becomes ready"); return;
    }
    SendMessageW(start, BM_CLICK, 0, 0);
    if (allRejected) {
        const bool stopped = pumpUntil([&] { return !IsWindowEnabled(cancel) && app.testViewState().stage == L"failed"; }, std::chrono::seconds(60));
        check(stopped, "fully rejected batch returns idle and reports failure");
        check(!std::filesystem::exists(first) && !std::filesystem::exists(second), "fully rejected batch publishes no output");
        if (stopped) {
            check(app.testViewState().status.find(L"first.mp4：色彩配置不受支持") != std::wstring::npos, "fully rejected batch reports a file and reason");
            check(app.testViewState().status.find(L"拒绝 2 个") != std::wstring::npos, "fully rejected batch reports rejection count");
            const HWND button = GetDlgItem(window, 1019);
            check(button && IsWindowVisible(button), "rejection details button becomes visible");
            if (button) SendMessageW(button, BM_CLICK, 0, 0);
            HWND details = nullptr;
            EnumThreadWindows(GetCurrentThreadId(), findRejectionWindow, reinterpret_cast<LPARAM>(&details));
            check(details != nullptr, "rejection details window opens");
            if (details) {
                const HWND text = FindWindowExW(details, nullptr, L"EDIT", nullptr);
                const int length = text ? GetWindowTextLengthW(text) : 0;
                std::wstring contents(static_cast<std::size_t>(length) + 1, L'\0');
                if (text) GetWindowTextW(text, contents.data(), length + 1);
                check(contents.find(L"first.mp4") != std::wstring::npos && contents.find(L"second.mp4") != std::wstring::npos,
                      "rejection details show every file");
                DestroyWindow(details);
            }
        }
        return;
    }
    if (cancelBatch) {
        const bool started = pumpUntil([&] { return app.testViewState().outputFrames > 0; }, std::chrono::seconds(60));
        check(started, "batch produces a frame before cancellation");
        if (!started) return;
        SendMessageW(cancel, BM_CLICK, 0, 0);
        const bool stopped = pumpUntil([&] { return !IsWindowEnabled(cancel); }, std::chrono::seconds(60));
        check(stopped, "cancelled batch returns to idle");
        check(!std::filesystem::exists(first) && !std::filesystem::exists(second), "cancelled batch does not publish incomplete outputs");
        if (stopped) {
            check(app.testViewState().stage == L"cancelled", "batch reports cancelled state");
            check(app.testViewState().status.find(L"1 个任务仍在队列") != std::wstring::npos, "batch reports remaining queued task");
        }
        return;
    }
    const bool completed = pumpUntil([&] { return std::filesystem::is_regular_file(first) &&
        (expectRejection || std::filesystem::is_regular_file(second)) && !IsWindowEnabled(cancel); }, std::chrono::seconds(120));
    if (!completed) std::cerr << "Batch status: " << utf8(app.testViewState().status) << '\n';
    check(completed, "batch publishes both files and returns idle");
    if (completed) {
        check(app.testViewState().stage == (expectRejection ? L"failed" : L"completed"), "batch reports the correct final state");
        if (expectRejection) {
            check(!std::filesystem::exists(second), "rejected input has no output");
            check(app.testViewState().status.find(L"拒绝 1 个") != std::wstring::npos, "batch reports rejection count");
            check(app.testViewState().status.find(L"second.mp4：色彩配置不受支持") != std::wstring::npos, "batch reports rejected file and reason");
        }
    }
}
#endif
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name = L"RifeLoveNativeControlTest_" + std::to_wstring(GetCurrentProcessId());
    const HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) {
        std::cerr << "FAIL: cannot create isolated Windows desktop (" << GetLastError() << ")\n";
        if (isolated) CloseDesktop(isolated);
        return 1;
    }

    {
        NativeApp app;
        check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "create native window");
        HWND window = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), findWindow, reinterpret_cast<LPARAM>(&window));
        check(window != nullptr, "find native window on isolated desktop");
        if (window) {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            const bool denoiseRun = (argc == 4 || argc == 5) &&
                (std::wstring(argv[1]) == L"--denoise" || std::wstring(argv[1]) == L"--denoise-reject" ||
                 std::wstring(argv[1]) == L"--dual" || std::wstring(argv[1]) == L"--dual-blur");
            checkRestorationControls(app, window, denoiseRun);
            const HWND pause = GetDlgItem(window, 1023);
            check(pause && IsWindowVisible(pause), "pause/resume button is visible");
            if (pause) {
                wchar_t label[32]{};
                GetWindowTextW(pause, label, static_cast<int>(std::size(label)));
                check(std::wstring(label) == L"暂停", "idle pause/resume button starts as pause");
                check(IsWindowEnabled(pause) == FALSE, "pause/resume is disabled while no queue is running");
            }
#endif
            const std::array<int, 6> ids{1005, 1006, 1012, 1024, 1017, 1018};
            for (const int id : ids) {
                const HWND combo = GetDlgItem(window, id);
                check(combo != nullptr, "find combo box");
                if (!combo) continue;
                RECT field{}, dropped{};
                GetWindowRect(combo, &field);
                const LRESULT result = SendMessageW(combo, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropped));
                const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
                const int itemHeight = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
                check(result != CB_ERR && count > 0 && itemHeight > 0, "read combo dimensions and items");
                const int visibleItems = std::min(count, 3);
                check(dropped.bottom - dropped.top >= field.bottom - field.top + itemHeight * visibleItems,
                      "drop-down has room for its options");
                if (IsWindowEnabled(combo)) {
                    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
                    check(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != FALSE, "open combo drop-down");
                    COMBOBOXINFO info{};
                    info.cbSize = sizeof(info);
                    if (GetComboBoxInfo(combo, &info) && info.hwndList) {
                        RECT list{};
                        GetWindowRect(info.hwndList, &list);
                        check(list.bottom - list.top >= itemHeight * visibleItems,
                              "open list exposes multiple complete items");
                    } else check(false, "find combo list window");
                    SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
                    check(SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) == FALSE, "close combo drop-down");
                }
                if (count > 1) {
                    SendMessageW(combo, CB_SETCURSEL, count - 1, 0);
                    check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == count - 1, "select last combo item");
                }
            }

            const HWND advanced = GetDlgItem(window, 1016);
            check(advanced != nullptr, "find advanced button");
            if (advanced) {
                SendMessageW(advanced, BM_CLICK, 0, 0);
                wchar_t text[64]{};
                GetWindowTextW(advanced, text, static_cast<int>(std::size(text)));
                check(std::wstring(text).find(L"收起") != std::wstring::npos, "advanced button opens settings");
                SendMessageW(advanced, BM_CLICK, 0, 0);
                GetWindowTextW(advanced, text, static_cast<int>(std::size(text)));
                check(std::wstring(text).find(L"参数") != std::wstring::npos, "advanced button closes settings");
            }
            const HWND combo = GetDlgItem(window, 1005);
            const HWND source = GetDlgItem(window, 1002);
            const HWND start = GetDlgItem(window, 1009);
            if (combo) {
                SendMessageW(combo, CB_SETCURSEL, 0, 0);
                SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
                SendMessageW(combo, WM_KEYUP, VK_DOWN, 0);
                check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "keyboard changes combo selection");
                RECT field{};
                GetClientRect(combo, &field);
                const LPARAM point = MAKELPARAM(8, (field.bottom - field.top) / 2);
                SendMessageW(combo, WM_LBUTTONDOWN, MK_LBUTTON, point);
                const bool openedByMouse = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
                SendMessageW(combo, WM_LBUTTONUP, 0, point);
                SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
                check(openedByMouse, "mouse press opens combo list");
            }
            const bool processingRun = (argc == 4 || argc == 5) && (std::wstring(argv[1]) == L"--processing" || std::wstring(argv[1]) == L"--cancel" || std::wstring(argv[1]) == L"--upscale-youtube" || std::wstring(argv[1]) == L"--denoise" || std::wstring(argv[1]) == L"--denoise-reject" || std::wstring(argv[1]) == L"--pause-resume" || std::wstring(argv[1]) == L"--dual" || std::wstring(argv[1]) == L"--dual-blur");
            const bool batchRun = argc == 3 && (std::wstring(argv[1]) == L"--batch" || std::wstring(argv[1]) == L"--batch-reject" || std::wstring(argv[1]) == L"--batch-cancel" || std::wstring(argv[1]) == L"--batch-all-rejected" || std::wstring(argv[1]) == L"--batch-youtube");
            const bool switchRun = argc == 4 && std::wstring(argv[1]) == L"--switch";
            if (combo && source && start)
                checkControlRendering(window, combo, source, start, argc > 1 && !processingRun && !batchRun && !switchRun ? std::filesystem::path(argv[1]) : std::filesystem::path{});
#ifndef RIFELOVE_NATIVE_HAS_PROCESSING
            if (!processingRun && !batchRun && !switchRun) checkPathFields(window, argc != 3);
#endif
            if (argc == 3 && !processingRun && !batchRun && !switchRun)
                check(captureClient(window, std::filesystem::path(argv[2])), "capture complete client on isolated desktop");
#ifndef RIFELOVE_NATIVE_HAS_PROCESSING
            if (argc == 3 && !processingRun && !batchRun && !switchRun) checkPathFields(window);
#endif
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            if (!processingRun && !batchRun && !switchRun) checkTaskHistory(app, window);
            if (switchRun) checkRapidSourceSwitch(app, window, std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            if (processingRun && std::wstring(argv[1]) == L"--denoise-reject") checkDenoiseColorRejection(app, window, std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            else if (processingRun && std::wstring(argv[1]) == L"--pause-resume") checkPauseResume(app, window, std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
            else if (processingRun) checkProcessing(app, window, std::filesystem::path(argv[2]),
                std::filesystem::path(argv[3]), argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{}, std::wstring(argv[1]) == L"--cancel", std::wstring(argv[1]) == L"--upscale-youtube", std::wstring(argv[1]) == L"--denoise" || std::wstring(argv[1]) == L"--dual" || std::wstring(argv[1]) == L"--dual-blur", std::wstring(argv[1]) == L"--dual" || std::wstring(argv[1]) == L"--dual-blur", std::wstring(argv[1]) == L"--dual-blur");
            if (batchRun) checkBatchProcessing(app, window, std::filesystem::path(argv[2]), std::wstring(argv[1]) == L"--batch-reject", std::wstring(argv[1]) == L"--batch-cancel", std::wstring(argv[1]) == L"--batch-all-rejected", std::wstring(argv[1]) == L"--batch-youtube");
#else
            if (processingRun) check(false, "processing test requires the media/ONNX application service");
#endif

            RECT rect{};
            GetWindowRect(window, &rect);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
                check(rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top &&
                      rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom,
                      "initial window fits monitor work area");
            DestroyWindow(window);
        }
    }

    // These checks simulate layout dimensions; actual per-monitor DPI changes
    // still require an interactive Windows display acceptance pass.
    for (const float scale : {1.0F, 1.25F, 1.5F}) {
        const float height = 680.0F / scale;
        const auto layout = NativeLayout::from(1366.0F / scale, height);
        check(layout.resourceButtonsTop + 72.0F <= height, "resource buttons fit a short scaled window");
        check(layout.dock + 70.0F <= height, "action buttons fit a short scaled window");
        check(layout.right > layout.split && layout.split > layout.left, "scaled columns retain positive width");
        check(layout.right - 330.0F > layout.left && layout.right - 254.0F <= layout.right && layout.right - layout.left - 360.0F > 0,
            "pause button and status text retain room at high DPI");
    }

    SetThreadDesktop(original);
    CloseDesktop(isolated);
    if (failures) return 1;
    std::cout << "Native controls: drop-down geometry, selection, button behavior and rendering passed.\n";
    return 0;
}

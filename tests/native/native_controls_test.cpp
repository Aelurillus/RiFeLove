#include "native_app.hpp"
#include "native_renderer.hpp"
#include "native_model_center_test.hpp"
#include "native_data_directory_test.hpp"
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "native_comparison_test.hpp"
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

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
    constexpr int width = 420, height = 234;
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
    SetTextColor(dc, RGB(244, 12, 222));
    SetBkMode(dc, OPAQUE);
    const HGDIOBJ expectedFont = GetCurrentObject(dc, OBJ_FONT);
    const auto checkDcState = [&] {
        check(GetCurrentObject(dc, OBJ_FONT) == expectedFont && GetTextColor(dc) == RGB(244, 12, 222) &&
              GetBkMode(dc) == OPAQUE, "control drawing preserves caller font and text DC state");
    };

    SetViewportOrgEx(dc, 16, 12, nullptr);
    SendMessageW(combo, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    checkDcState();
    SetViewportOrgEx(dc, 16, 56, nullptr);
    DRAWITEMSTRUCT button{};
    button.CtlType = ODT_BUTTON;
    button.CtlID = 1002;
    button.hwndItem = sourceButton;
    button.hDC = dc;
    button.rcItem = RECT{0, 0, 300, 34};
    SendMessageW(parent, WM_DRAWITEM, button.CtlID, reinterpret_cast<LPARAM>(&button));
    checkDcState();
    SetViewportOrgEx(dc, 16, 100, nullptr);
    button.CtlID = 1009;
    button.hwndItem = startButton;
    button.itemState = ODS_DISABLED;
    SendMessageW(parent, WM_DRAWITEM, button.CtlID, reinterpret_cast<LPARAM>(&button));
    checkDcState();
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
    checkDcState();
    SetViewportOrgEx(dc, 16, 188, nullptr);
    button.CtlID = 1002;
    button.hwndItem = sourceButton;
    button.itemState = ODS_SELECTED;
    SendMessageW(parent, WM_DRAWITEM, button.CtlID, reinterpret_cast<LPARAM>(&button));
    checkDcState();
    SetViewportOrgEx(dc, 0, 0, nullptr);

    check(GetPixel(dc, 30, 66) == RGB(145, 224, 192), "enabled button uses accent fill");
    check(GetPixel(dc, 30, 110) == RGB(38, 49, 63), "disabled button uses disabled fill");
    check(GetPixel(dc, 30, 154) == RGB(36, 66, 82), "selected drop-down item uses selected fill");
    check(GetPixel(dc, 30, 198) == RGB(118, 202, 170), "pressed button has distinct visual feedback");
    if (!output.empty()) check(writeBitmap(output, info.bmiHeader, pixels), "write off-screen visual evidence");
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void checkPathTooltip(HWND edit, const std::wstring& expected) {
    const auto tooltip = reinterpret_cast<HWND>(GetPropW(edit, L"RifeLovePathTooltip"));
    check(tooltip && IsWindow(tooltip), "path field owns a native hover tooltip");
    if (!tooltip) return;
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.hwnd = GetParent(edit);
    tool.uId = reinterpret_cast<UINT_PTR>(edit);
    check(SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE &&
        (tool.uFlags & (TTF_IDISHWND | TTF_SUBCLASS)) == (TTF_IDISHWND | TTF_SUBCLASS),
        "tooltip tracks the full edit hit target");
    NMTTDISPINFOW info{};
    info.hdr.hwndFrom = tooltip;
    info.hdr.code = TTN_GETDISPINFOW;
    SendMessageW(GetParent(edit), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&info));
    check(info.lpszText && std::wstring(info.lpszText) == expected,
        "hover tooltip exposes the complete current path without shortening");
}

void checkPathFields(NativeApp& app, HWND window, bool clearAfter = true) {
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
    checkPathTooltip(source, sourcePath);
    checkPathTooltip(output, outputPath);
    check(app.testViewState().sourcePath.find(std::filesystem::path(sourcePath).filename().wstring()) == 0,
        "source summary starts with the selected filename");
    check(app.testViewState().outputPath.find(std::filesystem::path(outputPath).filename().wstring()) == 0,
        "output summary starts with the output filename");
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

#endif

std::wstring comboItem(HWND combo, int index) {
    if (!combo || index < 0) return {};
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));
    return value;
}

void checkFrameRateOptions(HWND window) {
    const HWND rate = GetDlgItem(window, 1005);
    check(rate != nullptr, "find target frame rate selector");
    if (!rate) return;
    const std::array<std::wstring_view, 10> expected{
        L"30 FPS", L"60 FPS", L"120 FPS", L"144 FPS", L"240 FPS",
        L"300 FPS", L"360 FPS", L"480 FPS", L"600 FPS", L"自定义有理数"};
    check(SendMessageW(rate, CB_GETCOUNT, 0, 0) == static_cast<LRESULT>(expected.size()),
        "frame rate selector exposes standard, 300+ FPS and custom rational modes");
    for (std::size_t index = 0; index < expected.size(); ++index)
        check(comboItem(rate, static_cast<int>(index)) == expected[index],
            "frame rate option label matches the supported target rate");
    check(SendMessageW(rate, CB_SETCURSEL, static_cast<WPARAM>(expected.size() - 1), 0) != CB_ERR,
        "custom rational frame rate remains selectable after high-rate presets");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
    const HWND custom = GetDlgItem(window, 1011);
    wchar_t className[32]{};
    if (custom) GetClassNameW(custom, className, static_cast<int>(std::size(className)));
    check(custom && std::wstring(className) == L"Edit" && IsWindowVisible(custom),
          "custom rational edit appears for the final selector item");
    SendMessageW(rate, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
    check(custom && !IsWindowVisible(custom), "custom rational edit hides again for standard frame rates");
}

void checkPlatformPresets(NativeApp& app, HWND window) {
    const HWND rate = GetDlgItem(window, 1005), encoder = GetDlgItem(window, 1006);
    const HWND platform = GetDlgItem(window, 1012), custom = GetDlgItem(window, 1011);
    const HWND blur = GetDlgItem(window, 1013), shutter = GetDlgItem(window, 1014);
    const HWND samples = GetDlgItem(window, 1015), output = GetDlgItem(window, 1003);
    check(rate && encoder && platform && custom && blur && shutter && samples && output,
        "find controls for platform preset regression");
    if (!rate || !encoder || !platform || !custom || !blur || !shutter || !samples || !output) return;
    const auto readText = [](HWND control) {
        const int length = GetWindowTextLengthW(control);
        std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(control, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(length));
        return value;
    };
    const auto choose = [&](HWND control, int index) {
        check(SendMessageW(control, CB_SETCURSEL, index, 0) != CB_ERR, "select an export preset setting");
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(control), CBN_SELCHANGE), reinterpret_cast<LPARAM>(control));
    };
    const auto setBlur = [&](bool enabled) {
        SendMessageW(blur, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED), reinterpret_cast<LPARAM>(blur));
    };
    const int originalRate = static_cast<int>(SendMessageW(rate, CB_GETCURSEL, 0, 0));
    const int originalEncoder = static_cast<int>(SendMessageW(encoder, CB_GETCURSEL, 0, 0));
    const auto originalCustom = readText(custom), originalOutput = readText(output);
    const auto originalShutter = readText(shutter), originalSamples = readText(samples);
    const bool originalBlur = SendMessageW(blur, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetWindowTextW(output, L"C:\\RifeLove-preset-regression\\master.mkv");
    choose(encoder, 2);
    SetWindowTextW(shutter, L"270");
    SetWindowTextW(samples, L"8");
    setBlur(true);
    for (const int index : {5, 6, 7, 8}) {
        choose(rate, index);
        choose(platform, 1);
        const auto copy = app.testExportSettings();
        check(copy.targetRate == rifelove::Rational(60) && copy.motionBlur.enabled &&
            copy.motionBlur.shutterAngleDegrees == 180 && copy.motionBlur.sampleCount == 4,
            "60 FPS platform copy defaults to editable 180 degree four-sample exposure");
        choose(platform, 0);
        const auto master = app.testExportSettings();
        const std::array<int, 4> highRates{300, 360, 480, 600};
        check(master.targetRate == rifelove::Rational(highRates[static_cast<std::size_t>(index - 5)]) &&
            master.encoding.encoder == "libopenh264" && master.motionBlur.enabled &&
            master.motionBlur.shutterAngleDegrees == 270 && master.motionBlur.sampleCount == 8,
            "returning to master restores high rate, encoder and independent exposure settings");
        check(std::filesystem::path(readText(output)).extension() == L".mkv", "master restores the MKV output container");
    }
    choose(rate, 9);
    SetWindowTextW(custom, L"300000/1001");
    choose(platform, 1);
    choose(platform, 0);
    check(app.testExportSettings().targetRate == rifelove::Rational(300000, 1001) && readText(custom) == L"300000/1001",
        "master restores the exact custom rational rate");

    const std::array<int, 5> copyRates{60, 60, 120, 30, 60};
    const std::array<std::int64_t, 5> bitrates{12'000'000, 20'000'000, 40'000'000, 20'000'000, 20'000'000};
    const std::array<int, 5> gops{30, 120, 240, 60, 120};
    for (int index = 1; index <= 5; ++index) {
        choose(platform, index);
        const auto settings = app.testExportSettings();
        const auto offset = static_cast<std::size_t>(index - 1);
        check(settings.targetRate == rifelove::Rational(copyRates[offset]) && settings.encoding.frameRate == settings.targetRate &&
            settings.encoding.encoder == "auto" && settings.encoding.bitRate == bitrates[offset] && settings.encoding.gopSize == gops[offset],
            "platform controls pass actual frame rate, encoder, bitrate and GOP to the common job settings");
        check(settings.motionBlur.enabled == (copyRates[offset] == 60), "only the 60 FPS copy presets initially enable natural exposure");
        check(std::filesystem::path(readText(output)).extension() == L".mp4", "platform copy selects MP4 output");
        check(app.testViewState().platformSummary.find(std::to_wstring(copyRates[offset]) + L" FPS") != std::wstring::npos,
            "platform explanation reports the actual selected output rate");
        if (copyRates[offset] == 60)
            check(app.testViewState().platformSummary.find(L"240 FPS") == std::wstring::npos,
                "60 FPS output is not described as 240 FPS playback");
        if (index == 3)
            check(app.testViewState().platformSummary.find(L"历史") != std::wstring::npos &&
                app.testViewState().platformSummary.find(L"实测") != std::wstring::npos,
                "Bilibili 120 FPS retains its historical experimental qualification");
    }
    choose(platform, 2);
    choose(rate, 9);
    SetWindowTextW(custom, L"30000/1001");
    choose(encoder, 1);
    SetWindowTextW(shutter, L"90");
    SetWindowTextW(samples, L"6");
    setBlur(false);
    choose(platform, 1);
    choose(platform, 2);
    auto edited = app.testExportSettings();
    check(edited.targetRate == rifelove::Rational(30000, 1001) && edited.encoding.encoder == "h264_nvenc" &&
        edited.encoding.gopSize == 60 && !edited.motionBlur.enabled && readText(shutter) == L"90" && readText(samples) == L"6",
        "platform revisits retain manual rate, encoder, disabled exposure and edited parameters");
    setBlur(true);
    edited = app.testExportSettings();
    check(edited.motionBlur.shutterAngleDegrees == 90 && edited.motionBlur.sampleCount == 6,
        "manual exposure values are passed to the same settings builder as processing jobs");
    check(app.testViewState().platformSummary.find(L"30000/1001 FPS") != std::wstring::npos,
        "manual rational rate is visible instead of the preset's nominal 60 FPS");
    // Leave each platform preset at its documented default for later media
    // fixtures in this same process; no production reset-only API is needed.
    choose(rate, 1);
    choose(encoder, 0);
    SetWindowTextW(shutter, L"180");
    SetWindowTextW(samples, L"4");
    choose(platform, 1);
    choose(rate, 9);
    SetWindowTextW(custom, L"30000/1001");
    const auto fractionalYoutube = app.testExportSettings();
    check(fractionalYoutube.encoding.bitRate == 8'000'000 && fractionalYoutube.encoding.gopSize == 15,
        "fractional 29.97 FPS uses standard-rate YouTube bitrate and GOP rather than falling back to 60 FPS");
    choose(rate, 1);
    choose(platform, 0);
    choose(rate, originalRate);
    choose(encoder, originalEncoder);
    SetWindowTextW(custom, originalCustom.c_str());
    SetWindowTextW(shutter, originalShutter.c_str());
    SetWindowTextW(samples, originalSamples.c_str());
    setBlur(originalBlur);
    SetWindowTextW(output, originalOutput.c_str());
}

void checkLongComboPopup(HWND window) {
    const HWND combo = GetDlgItem(window, 1012);
    if (!combo) { check(false, "find platform selector for long popup labels"); return; }
    const wchar_t label[] = L"YouTube · 60 FPS 上传副本 / 保留完整设置名称与质量配置";
    const LRESULT inserted = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    check(inserted >= 0, "add a long selector label for popup width regression");
    if (inserted < 0) return;
    // Resize notification recomputes actual native control geometry without
    // moving focus or touching the user's desktop.
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, 0);
    HDC dc = GetDC(combo);
    if (dc) {
        const HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)));
        SIZE extent{};
        GetTextExtentPoint32W(dc, label, static_cast<int>(std::size(label) - 1), &extent);
        SelectObject(dc, old);
        ReleaseDC(combo, dc);
        const UINT dpi = GetDpiForWindow(window);
        const float scale = static_cast<float>(dpi ? dpi : 96U) / 96.0F;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        check(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE,
              "read monitor bounds for popup width constraint");
        const int available = std::max(1, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - static_cast<int>(16 * scale));
        const int required = std::min(static_cast<int>(extent.cx) + static_cast<int>(48 * scale), available);
        check(SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0) >= required,
              "popup expands to fit long names with DPI-scaled padding");
    } else check(false, "measure selector label using the control's actual font");
    SendMessageW(combo, CB_DELETESTRING, static_cast<WPARAM>(inserted), 0);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, 0);
}

void checkOutputFieldAlignment(HWND window);

void checkQualityFieldSpacing(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    const auto px = [&](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96U), 96); };
    const std::array<int, 3> controls{1024, 1017, 1018};
    const std::array<int, 3> labelTops{331, 397, 463};
    for (std::size_t index = 0; index < controls.size(); ++index) {
        const HWND control = GetDlgItem(window, controls[index]);
        check(control != nullptr, "find restoration field for label spacing");
        if (!control) continue;
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        check(rect.top >= px(labelTops[index] + 22),
            "restoration selector leaves the entire painted label and spacing visible");
        check(rect.bottom <= px(labelTops[index] + 64),
            "restoration selector does not cover the following label or section divider");
    }
}

void checkAspectPreservingOutputTargets() {
    using rifelove::native_desktop::nativeOutputTarget;
    struct Example { int sourceWidth, sourceHeight, selection, width, height; bool usable, nominal; };
    const std::array<Example, 13> examples{{
        {1920, 1080, 1, 2560, 1440, true, true},
        {1920, 1080, 2, 3840, 2160, true, true},
        {1080, 1920, 1, 1440, 2560, true, true},
        {1440, 1080, 1, 1920, 1440, true, true},
        {1440, 1080, 2, 2880, 2160, true, true},
        {1080, 1440, 1, 1440, 1920, true, true},
        {1080, 1080, 2, 2160, 2160, true, true},
        {3440, 1440, 2, 5160, 2160, true, true},
        {2560, 1080, 1, 3456, 1458, true, false},
        {1366, 768, 1, 2732, 1536, true, false},
        {1366, 768, 2, 4098, 2304, false, false},
        {1280, 720, 2, 3840, 2160, false, true},
        {32768, 4, 1, 11796480, 1440, false, true},
    }};
    for (const auto& example : examples) {
        const auto result = nativeOutputTarget(example.sourceWidth, example.sourceHeight, example.selection);
        check(result.width == example.width && result.height == example.height,
            "size preset chooses the expected even dimensions for the source ratio");
        check(result.aspectMatches && result.dimensionsEven,
            "size preset preserves exact source ratio with even encoder dimensions");
        check(result.usable() == example.usable && result.nominalSize == example.nominal,
            "size preset reports x2/encoder limits and approximate nominal sizes honestly");
    }
    check(!nativeOutputTarget(0, 1080, 1).usable() && !nativeOutputTarget(1920, -1, 1).usable(),
        "invalid or unknown source dimensions cannot enable a size preset");
    check(!nativeOutputTarget(1920, 1080, 0).selected && !nativeOutputTarget(1920, 1080, 3).selected,
        "original-size and unknown selections do not impose a target");
    const auto oversized = nativeOutputTarget(std::numeric_limits<int>::max(), 1, 2);
    check(!oversized.usable() && !oversized.dimensionsSupported,
        "extreme source ratios cannot overflow into a valid encoding target");
}

void checkNarrowImportLayout(HWND window, const std::filesystem::path& screenshot = {}) {
    RECT original{};
    GetWindowRect(window, &original);
    const UINT dpi = GetDpiForWindow(window);
    const auto px = [&](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96U), 96); };
    SetWindowPos(window, nullptr, original.left, original.top, px(850), px(600), SWP_NOZORDER | SWP_NOACTIVATE);
    const HWND sourceButton = GetDlgItem(window, 1002);
    const HWND sourceEdit = GetDlgItem(window, 1001);
    RECT button{}, input{};
    GetWindowRect(sourceButton, &button);
    GetWindowRect(sourceEdit, &input);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&button), 2);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&input), 2);
    check(button.top >= px(144), "narrow import actions leave the card title unobstructed");
    check(input.top >= button.bottom + px(8), "narrow import field has spacing below its actions");
    const HWND folder = GetDlgItem(window, 1022);
    if (folder && IsWindowVisible(folder)) {
        RECT folderRect{};
        GetWindowRect(folder, &folderRect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&folderRect), 2);
        check(folderRect.left >= button.right + px(6) && folderRect.top == button.top,
              "file and folder buttons retain separate hit targets in narrow windows");
    }
    checkOutputFieldAlignment(window);
    checkQualityFieldSpacing(window);
    if (!screenshot.empty()) check(captureClient(window, screenshot), "capture narrow import layout");
    SetWindowPos(window, nullptr, original.left, original.top, original.right - original.left,
                 original.bottom - original.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void checkOutputFieldAlignment(HWND window) {
    const HWND output = GetDlgItem(window, 1003);
    RECT field{};
    const UINT dpi = GetDpiForWindow(window);
    const float scale = static_cast<float>(dpi ? dpi : 96U) / 96.0F;
    RECT client{};
    GetClientRect(window, &client);
    const auto layout = NativeLayout::from(static_cast<float>(client.right) / scale,
        static_cast<float>(client.bottom) / scale);
    const auto px = [&](float value) { return static_cast<int>(static_cast<float>(static_cast<int>(value)) * scale); };
    const auto approximatelyEqual = [](LONG actual, int expected) { return std::abs(actual - expected) <= 2; };
    GetWindowRect(output, &field);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&field), 2);
    check(field.top == static_cast<int>(394 * scale) && field.bottom <= static_cast<int>(436 * scale),
          "output edit remains inside its painted rounded field");
    check(approximatelyEqual(field.left, px(layout.exportContentLeft() + 10)) &&
        approximatelyEqual(field.right, px(layout.right - 30)),
        "output edit keeps equal insets inside the shared export field");
    for (const int id : {1012, 1005, 1006}) {
        RECT selector{};
        GetWindowRect(GetDlgItem(window, id), &selector);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&selector), 2);
        check(approximatelyEqual(selector.left, px(layout.exportContentLeft())) && approximatelyEqual(selector.right, px(layout.right - 20)),
            "export selectors align to the card's shared content margins");
    }
    RECT browse{};
    GetWindowRect(GetDlgItem(window, 1004), &browse);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&browse), 2);
    check(approximatelyEqual(browse.right, px(layout.right - 20)) && browse.right - browse.left <= px(120) + 1,
        "output location button is compact and right aligned");
    check(browse.top >= field.bottom + px(20), "path summary has space above the output location button");
}

void checkResponsiveInteraction(NativeApp& app, HWND window, const std::filesystem::path& screenshot = {}) {
    RECT original{}; GetWindowRect(window, &original);
    const UINT dpi = GetDpiForWindow(window);
    const auto px = [&](int value) { return MulDiv(value, static_cast<int>(dpi ? dpi : 96U), 96); };
    const float scale = static_cast<float>(dpi ? dpi : 96U) / 96.0F;
    const auto rect = [&](HWND control) {
        RECT value{}; GetWindowRect(control, &value);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&value), 2); return value;
    };
    const auto withinViewport = [&](HWND control) {
        RECT client{}; GetClientRect(window, &client);
        const auto layout = NativeLayout::from(client.right / scale, client.bottom / scale, app.testViewState().scrollOffset);
        const RECT field = rect(control);
        return IsWindowVisible(control) && field.top >= px(static_cast<int>(layout.contentTop)) &&
            field.bottom <= px(static_cast<int>(layout.contentBottom)) + 1 &&
            field.left >= px(static_cast<int>(layout.left)) && field.right <= px(static_cast<int>(layout.right)) + 1;
    };
    const HWND source = GetDlgItem(window, 1002), sourceEdit = GetDlgItem(window, 1001), rate = GetDlgItem(window, 1005);
    const HWND blur = GetDlgItem(window, 1013), advanced = GetDlgItem(window, 1016);
    const bool priorBlur = SendMessageW(blur, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool priorAdvanced = app.testViewState().advancedExpanded;
    const auto priorRate = SendMessageW(rate, CB_GETCURSEL, 0, 0);
    const auto scrollTop = [&] { SendMessageW(window, WM_VSCROLL, SB_TOP, 0); };
    for (const int width : {480, 600, 700, 790, 800, 850}) {
        SetWindowPos(window, nullptr, original.left, original.top, px(width), px(600), SWP_NOZORDER | SWP_NOACTIVATE);
        scrollTop();
        RECT client{}; GetClientRect(window, &client);
        const auto layout = NativeLayout::from(client.right / scale, client.bottom / scale);
        if (width == 480 && !screenshot.empty()) {
            auto top = screenshot; top.replace_filename(top.stem().wstring() + L"-480-top" + top.extension().wstring());
            check(captureClient(window, top), "capture real 480-DIP HWND at the top of its settings viewport");
            SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0);
            auto bottom = screenshot; bottom.replace_filename(bottom.stem().wstring() + L"-480-bottom" + bottom.extension().wstring());
            check(captureClient(window, bottom), "capture real 480-DIP HWND at the bottom of its settings viewport");
            scrollTop();
        }
        check((width < 760 && layout.singleColumn) || (width >= 790 && layout.split > layout.left),
            "small HWND retains usable settings through the single/two-column transition");
        check(rect(source).right - rect(source).left >= px(100), "small window retains a full-size file button");
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        check(rect(GetDlgItem(window, 1022)).right - rect(GetDlgItem(window, 1022)).left >= px(96),
            "small window retains a usable folder import button");
#endif
        check(rect(GetDlgItem(window, 1003)).right - rect(GetDlgItem(window, 1003)).left >= px(width < 760 ? 270 : 160),
            "output field retains readable width rather than becoming a narrow second column");
        for (const int id : {1009, 1010, 1023}) {
            const auto button = rect(GetDlgItem(window, id));
            check(button.left >= px(static_cast<int>(layout.left)) && button.right <= client.right && button.bottom <= client.bottom,
                "fixed start, cancel and pause hit targets stay wholly inside the main area");
            check(IsWindowVisible(GetDlgItem(window, id)), "fixed task actions stay visible while settings scroll");
        }

        SetFocus(source);
        const auto beforeButtonWheel = app.testViewState().scrollOffset;
        SendMessageW(source, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        check(app.testViewState().scrollOffset > beforeButtonWheel, "wheel over a native button scrolls the settings viewport");
        SendMessageW(sourceEdit, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        check(app.testViewState().scrollOffset > beforeButtonWheel + 40, "wheel over an edit also scrolls the settings viewport");

        scrollTop(); SetFocus(source);
        const bool sizeEnabled = IsWindowEnabled(GetDlgItem(window, 1018));
        bool sawOutput = false, sawBrowse = false, sawRate = false, sawSize = false;
        for (int count = 0; count < 32; ++count) {
            check(app.testNavigateSettings(), "forward Tab navigation finds an enabled intended-visible control");
            const int id = GetDlgCtrlID(GetFocus());
            if (id == 1003 || id == 1004 || id == 1005 || id == 1018) {
                check(withinViewport(GetFocus()), "Tab automatically scrolls hidden export controls fully into view");
                sawOutput |= id == 1003; sawBrowse |= id == 1004; sawRate |= id == 1005;
                sawSize |= id == 1018;
            }
            check(id != 1014 && id != 1015 && id != 1011, "Tab skips collapsed exposure and inactive custom-rate inputs");
            if (sawOutput && sawBrowse && sawRate) break;
        }
        check(sawOutput && sawBrowse && sawRate, "Tab reaches target FPS, output path and output browse in small windows");
        check(!sizeEnabled || sawSize, "Tab reaches the output-size selector when it is enabled");

        // Closed fields scroll the page; open lists retain their own wheel
        // behavior and cannot unexpectedly move the surrounding settings.
        SetFocus(rate); SendMessageW(rate, CB_SHOWDROPDOWN, FALSE, 0);
        const auto selected = SendMessageW(rate, CB_GETCURSEL, 0, 0);
        const auto beforeComboWheel = app.testViewState().scrollOffset;
        SendMessageW(rate, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        check(app.testViewState().scrollOffset > beforeComboWheel, "wheel over a closed drop-down scrolls the page");
        check(SendMessageW(rate, CB_GETCURSEL, 0, 0) == selected, "closed drop-down wheel cannot silently change target FPS");
        SetFocus(rate);
        SendMessageW(rate, CB_SHOWDROPDOWN, TRUE, 0);
        check(SendMessageW(rate, CB_GETDROPPEDSTATE, 0, 0), "small-window drop-down can open after automatic scrolling");
        const auto openScroll = app.testViewState().scrollOffset;
        SendMessageW(rate, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
        check(app.testViewState().scrollOffset == openScroll, "wheel over an open drop-down stays with its list");
        SendMessageW(rate, CB_SHOWDROPDOWN, FALSE, 0);
        SendMessageW(rate, CB_SETCURSEL, priorRate, 0);

        bool returned = false;
        for (int count = 0; count < 32; ++count) {
            check(app.testNavigateSettings(true), "reverse Tab navigation reaches earlier settings");
            if (GetFocus() == source) { returned = true; break; }
        }
        check(returned && withinViewport(source) && app.testViewState().scrollOffset < 100,
            "Shift+Tab returns from export controls to the visible import actions");

        SendMessageW(blur, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED), reinterpret_cast<LPARAM>(blur));
        if (!app.testViewState().advancedExpanded) SendMessageW(advanced, BM_CLICK, 0, 0);
        SetFocus(source); bool sawShutter = false, sawSamples = false;
        for (int count = 0; count < 32; ++count) {
            app.testNavigateSettings(); const int id = GetDlgCtrlID(GetFocus());
            if (id == 1014 || id == 1015) {
                check(withinViewport(GetFocus()), "expanded exposure inputs scroll fully into view when reached by Tab");
                sawShutter |= id == 1014; sawSamples |= id == 1015;
            }
            if (sawShutter && sawSamples) break;
        }
        check(sawShutter && sawSamples, "Tab reaches both shutter and sampling controls after expanding parameters");
        const auto samples = rect(GetDlgItem(window, 1015));
        check(samples.right <= px(static_cast<int>(layout.split - 20)) + 2,
            "exposure sampling field stays inside its quality card at the two-column breakpoint");
        if (!priorAdvanced) SendMessageW(advanced, BM_CLICK, 0, 0);
        SendMessageW(blur, BM_SETCHECK, priorBlur ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED), reinterpret_cast<LPARAM>(blur));
        scrollTop();
    }
    SetWindowPos(window, nullptr, original.left, original.top, original.right - original.left,
        original.bottom - original.top, SWP_NOZORDER | SWP_NOACTIVATE);
    scrollTop(); SetFocus(source);
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
void checkMissingRestorationDescriptor(NativeApp& app, HWND window) {
    using namespace rifelove;
    native_test::ModelCenterTemporaryDirectory fixture;
    restoration::ModelPack temporal{}, spatial{};
    temporal.directory = fixture.root / L"temporal";
    spatial.directory = fixture.root / L"spatial";
    std::filesystem::create_directories(temporal.directory);
    std::filesystem::create_directories(spatial.directory);
    const auto wasProcessing = app.testViewState().processing;
    application::ProcessingJob job;
    job.expectedTemporalRestorationDescriptorSha256 = "unchanged-temporal";
    job.expectedRestorationDescriptorSha256 = "unchanged-spatial";
    check(!app.testCaptureRestorationDescriptors(job, &temporal, nullptr),
        "missing selected temporal descriptor is converted to a startup failure without throwing");
    check(IsWindow(window) && app.testViewState().processing == wasProcessing && app.testViewState().stage == L"failed" &&
        app.testViewState().status.find(L"恢复所选降噪/超分模型的 model.json") != std::wstring::npos &&
        app.testViewState().status.find(L"重新选择模型") != std::wstring::npos,
        "missing selected model keeps the desktop alive and provides recovery or reselection guidance");
    std::ofstream(temporal.directory / L"model.json", std::ios::binary) << "{}";
    check(!app.testCaptureRestorationDescriptors(job, &temporal, &spatial),
        "missing spatial descriptor is caught after a successful temporal descriptor read");
    check(job.expectedTemporalRestorationDescriptorSha256 == "unchanged-temporal" &&
        job.expectedRestorationDescriptorSha256 == "unchanged-spatial" &&
        job.temporalRestorationModelDirectory.empty() && job.restorationModelDirectory.empty(),
        "descriptor read failure does not leave a partially captured job for enqueueing");
    check(app.testCaptureRestorationDescriptors(job, nullptr, nullptr),
        "startup descriptor validation remains usable after a failed selection");
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
    app.testSetSourceDimensions(0, 0);
    check(comboItem(outputSize, 1).find(L"导入视频后") != std::wstring::npos && comboItem(outputSize, 1).find(L"0 × 0") == std::wstring::npos,
        "unknown source dimensions show a waiting-for-video label instead of an approximate zero-pixel target");
    const HWND platform = GetDlgItem(window, 1012);
    const auto previousPlatform = SendMessageW(platform, CB_GETCURSEL, 0, 0);
    SendMessageW(platform, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), reinterpret_cast<LPARAM>(platform));
    app.testSetSourceDimensions(1080, 1920);
    check(app.testExportSettings().encoding.bitRate == 12000000,
        "single-file portrait 1080p YouTube bitrate uses the output short edge rather than its 1920-pixel height");
    app.testSetSourceDimensions(1440, 1080);
    check(app.testExportSettings().encoding.bitRate == 12000000,
        "single-file 4:3 YouTube bitrate agrees with the same shared actual-output policy used for batch jobs");
    SendMessageW(platform, CB_SETCURSEL, previousPlatform, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1012, CBN_SELCHANGE), reinterpret_cast<LPARAM>(platform));
    app.testSetSourceDimensions(1440, 1080);
    check(comboItem(outputSize, 1).find(L"1920 × 1440") != std::wstring::npos &&
          comboItem(outputSize, 2).find(L"2880 × 2160") != std::wstring::npos,
        "4:3 source updates the actual output-size selector without offering stretched 16:9 output");
    app.testSetSourceDimensions(3440, 1440);
    check(comboItem(outputSize, 2).find(L"5160 × 2160") != std::wstring::npos,
        "ultrawide source updates the actual output-size selector at its original ratio");
    app.testSetSourceDimensions(2560, 1080);
    check(comboItem(outputSize, 1).starts_with(L"约 ") &&
          comboItem(outputSize, 1).find(L"3456 × 1458") != std::wstring::npos,
        "non-integral nominal preset displays approximate wording and exact output pixels");
    app.testSetSourceDimensions(1920, 1080);
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
        check(comboItem(temporal, denoiseIndex).find(L"固定 σ25") != std::wstring::npos &&
              app.testViewState().restorationSummary.find(L"固定 σ25") != std::wstring::npos,
            "FastDVDnet selector and summary explain that automatic off still uses fixed sigma 25");
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
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if ((argc == 4 || argc == 5) && std::wstring(argv[1]) == L"--comparison") {
        {
            rifelove::native_test::ModelCenterTemporaryDirectory profile;
            NativeApp app({}, profile.root / L"LocalAppData");
            check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "create owner for real-media comparison");
            if (const HWND owner = app.testMainWindow()) {
                rifelove::native_test::checkNativeComparisonFiles(check, pumpUntil, captureClient, owner,
                    std::filesystem::path(argv[2]), std::filesystem::path(argv[3]), argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{});
                DestroyWindow(owner);
            }
        }
        SetThreadDesktop(original); CloseDesktop(isolated);
        if (failures) return 1;
        std::cout << "Native comparison: real decode, matched time, adjacent frames, slider and EOF passed.\n";
        return 0;
    }
#endif

    rifelove::native_test::checkLocalModelPreferences(check);
    rifelove::native_test::checkNativeDataDirectories(check);
    checkAspectPreservingOutputTargets();
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (argc == 1) rifelove::native_test::checkLocalModelCenter(check);
#endif
    {
        rifelove::native_test::ModelCenterTemporaryDirectory profile;
        NativeApp app({}, profile.root / L"LocalAppData");
        check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "create native window");
        HWND window = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), findWindow, reinterpret_cast<LPARAM>(&window));
        check(window != nullptr, "find native window on isolated desktop");
        if (window) {
            checkFrameRateOptions(window);
            checkPlatformPresets(app, window);
            checkLongComboPopup(window);
            checkNarrowImportLayout(window);
            checkOutputFieldAlignment(window);
            checkQualityFieldSpacing(window);
            checkResponsiveInteraction(app, window, argc == 3 && argv[1][0] != L'-' ? std::filesystem::path(argv[2]) : std::filesystem::path{});
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            const bool denoiseRun = (argc == 4 || argc == 5) &&
                (std::wstring(argv[1]) == L"--denoise" || std::wstring(argv[1]) == L"--denoise-reject" ||
                 std::wstring(argv[1]) == L"--dual" || std::wstring(argv[1]) == L"--dual-blur");
            checkRestorationControls(app, window, denoiseRun);
            if (argc == 1) {
                checkMissingRestorationDescriptor(app, window);
                SendMessageW(GetDlgItem(window, 1025), BM_CLICK, 0, 0);
                const HWND comparison = app.testShowComparison();
                check(comparison && GetDlgItem(comparison, 1301) && GetDlgItem(comparison, 1302),
                    "main comparison action opens a reusable native window with source and result file selectors");
                if (comparison) SendMessageW(comparison, WM_CLOSE, 0, 0);
                check(IsWindow(window), "closing comparison preserves the main task window");
                rifelove::native_test::checkNativeComparison(check, pumpUntil, window);
            }
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
            if (!processingRun && !batchRun && !switchRun) checkPathFields(app, window, argc != 3);
#endif
            if (argc == 3 && !processingRun && !batchRun && !switchRun)
                check(captureClient(window, std::filesystem::path(argv[2])), "capture complete client on isolated desktop");
            if (argc == 3 && !processingRun && !batchRun && !switchRun) {
                auto narrow = std::filesystem::path(argv[2]);
                narrow.replace_filename(narrow.stem().wstring() + L"-narrow" + narrow.extension().wstring());
                checkNarrowImportLayout(window, narrow);
            }
#ifndef RIFELOVE_NATIVE_HAS_PROCESSING
            if (argc == 3 && !processingRun && !batchRun && !switchRun) checkPathFields(app, window);
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
    for (const float scale : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F}) {
        const float height = 680.0F / scale;
        const auto layout = NativeLayout::from(1366.0F / scale, height);
        check(layout.resourceButtonsTop + 72.0F <= height, "resource buttons fit a short scaled window");
        check(layout.dock + (layout.singleColumn ? 122.0F : 70.0F) <= height, "action buttons fit a short scaled window");
        check((layout.singleColumn ? layout.right == layout.split : layout.right > layout.split) && layout.split > layout.left,
            "scaled settings retain positive width in either layout mode");
        check(layout.right - 330.0F > layout.left && layout.right - 254.0F <= layout.right &&
            layout.right - layout.left - (layout.singleColumn ? 4.0F : 360.0F) > 0,
            "pause button and status text retain room at high DPI");
        if (layout.compactImport)
            check(layout.sourceActionsTop >= 144 && layout.sourceInputTop >= layout.sourceActionsTop + 42,
                  "scaled narrow import geometry keeps the title, actions and input separate");
    }

    SetThreadDesktop(original);
    CloseDesktop(isolated);
    if (failures) return 1;
    std::cout << "Native controls: drop-down geometry, selection, button behavior and rendering passed.\n";
    return 0;
}

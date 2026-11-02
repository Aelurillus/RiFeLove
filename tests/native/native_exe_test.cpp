#include <windows.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>

namespace {
struct FindWindowContext {
    DWORD processId;
    HWND result = nullptr;
    const wchar_t* className = L"RifeLoveNativeDesktopWindow";
};

BOOL CALLBACK findTargetWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (processId == context.processId && std::wstring(className) == context.className) {
        context.result = window;
        return FALSE;
    }
    return TRUE;
}

std::string utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

bool waitUntil(const std::function<bool()>& predicate, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        Sleep(50);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool captureTarget(HWND window, const std::filesystem::path& output) {
    RECT rect{};
    if (!GetClientRect(window, &rect)) return false;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
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
    // PrintWindow's default WM_PRINT paints the child HWNDs but omits this
    // application's Direct2D background on a private, non-displayed desktop.
    // Ask the actual EXE's WM_PRINTCLIENT renderer for the client background,
    // then compose its real direct children into that same DC. Never invent a
    // replacement background or read from the user's foreground desktop.
    const COLORREF marker = RGB(253, 1, 251);
    HBRUSH sentinel = CreateSolidBrush(marker);
    FillRect(dc, &rect, sentinel); DeleteObject(sentinel); GdiFlush();
    DWORD_PTR ignored = 0;
    bool painted = SendMessageTimeoutW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &ignored) != 0;
    GdiFlush();
    // A delivered cross-process message alone does not prove that its HDC was
    // usable by Direct2D. Refuse blank/flat output instead of certifying it.
    std::array<std::uint32_t, 8> colors{};
    std::size_t colorCount = 0, changed = 0, sampled = 0;
    const auto* bytes = static_cast<const unsigned char*>(pixels);
    constexpr std::uint32_t markerRgb = 0x00fd01fb;
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; pixel += 17) {
        const auto* rgb = bytes + pixel * 4;
        const std::uint32_t color = static_cast<std::uint32_t>(rgb[0]) | (static_cast<std::uint32_t>(rgb[1]) << 8) |
            (static_cast<std::uint32_t>(rgb[2]) << 16);
        ++sampled; if (color != markerRgb) ++changed;
        if (colorCount < colors.size() && std::find(colors.begin(), colors.begin() + colorCount, color) == colors.begin() + colorCount)
            colors[colorCount++] = color;
    }
    painted = painted && changed > sampled / 2 && colorCount == colors.size();
    if (!painted) std::cerr << "Actual EXE WM_PRINTCLIENT did not render a usable background across processes (changed "
        << changed << '/' << sampled << " sampled pixels, " << colorCount << "/8 distinct colors); no composite is claimed.\n";
    for (HWND child = painted ? GetWindow(window, GW_CHILD) : nullptr; child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT position{}; if (!GetWindowRect(child, &position)) { painted = false; break; }
        POINT origin{position.left, position.top}; ScreenToClient(window, &origin);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        const bool childPainted = SendMessageTimeoutW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
            PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &ignored) != 0;
        RestoreDC(dc, saved);
        if (!childPainted) { painted = false; break; }
    }
    GdiFlush();
    std::ofstream stream;
    if (painted) stream.open(output, std::ios::binary);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + info.bmiHeader.biSizeImage;
    if (painted && stream) {
        stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
        stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        stream.write(reinterpret_cast<const char*>(pixels), info.bmiHeader.biSizeImage);
    }
    const bool written = painted && static_cast<bool>(stream);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return written;
}

// Select a model by the text exposed by the target combo box instead of
// assuming that the optional model happens to remain at index 1.  The test
// runs in a separate process, so reading the selected combo text through
// GetWindowTextW keeps the check within User32's cross-process contract.
bool selectComboItemByLabel(HWND parent, int id, const wchar_t* label) {
    const HWND combo = GetDlgItem(parent, id);
    if (!combo || !label) {
        std::cerr << "Could not find combo control " << id << ".\n";
        return false;
    }
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (count <= 1) {
        std::cerr << "Combo control " << id << " contains " << count << " item(s); requested label is unavailable.\n";
        return false;
    }
    for (int index = 1; index < count; ++index) {
        if (SendMessageW(combo, CB_SETCURSEL, index, 0) == CB_ERR) continue;
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
        wchar_t text[512]{};
        // GetWindowTextW does not reliably retrieve owner-drawn combo text
        // across processes. WM_GETTEXT is marshalled by User32 and returns the
        // selected CBS_HASSTRINGS item from the target app.
        SendMessageW(combo, WM_GETTEXT, static_cast<WPARAM>(std::size(text)), reinterpret_cast<LPARAM>(text));
        std::cerr << "Combo control " << id << " item " << index << ": " << utf8(text) << "\n";
        if (std::wstring(text).find(label) != std::wstring::npos) return true;
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    std::cerr << "Combo control " << id << " has " << count << " items, but none matched the requested label.\n";
    return false;
}

std::wstring controlText(HWND window) {
    wchar_t value[4096]{};
    SendMessageW(window, WM_GETTEXT, static_cast<WPARAM>(std::size(value)), reinterpret_cast<LPARAM>(value));
    return value;
}

BOOL CALLBACK findAutomaticDenoiseDialog(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (processId == context.processId && std::wstring(name) == L"#32770" &&
        controlText(window) == L"实验性自动高斯降噪") {
        context.result = window; return FALSE;
    }
    return TRUE;
}

bool confirmAutomaticDenoise(HDESK desktop, DWORD processId, HWND parent) {
    const HWND button = GetDlgItem(parent, 1026);
    if (!button || !IsWindowEnabled(button) || controlText(button).find(L"关") == std::wstring::npos) {
        std::cerr << "Automatic-denoise control is missing, disabled or not initially off.\n"; return false;
    }
    // BM_CLICK via SendMessage would block this driver inside the modal dialog.
    // Post only to the child process on its private desktop, then inspect it.
    if (!PostMessageW(parent, WM_COMMAND, MAKEWPARAM(1026, BN_CLICKED), reinterpret_cast<LPARAM>(button))) return false;
    FindWindowContext dialog{processId};
    if (!waitUntil([&] {
        EnumDesktopWindows(desktop, findAutomaticDenoiseDialog, reinterpret_cast<LPARAM>(&dialog));
        return dialog.result != nullptr;
    }, std::chrono::seconds(10))) {
        std::cerr << "Experimental Gaussian-denoise explanation did not open.\n"; return false;
    }
    std::wstring explanation;
    EnumChildWindows(dialog.result, [](HWND child, LPARAM parameter) -> BOOL {
        auto& text = *reinterpret_cast<std::wstring*>(parameter); text += controlText(child); text += L'\n'; return TRUE;
    }, reinterpret_cast<LPARAM>(&explanation));
    const bool truthful = explanation.find(L"高斯") != std::wstring::npos &&
        explanation.find(L"颗粒") != std::wstring::npos && explanation.find(L"软件解码") != std::wstring::npos &&
        explanation.find(L"重放") != std::wstring::npos && explanation.find(L"固定 σ25") != std::wstring::npos;
    const HWND yes = GetDlgItem(dialog.result, IDYES);
    if (!truthful || !yes) {
        std::cerr << "Experimental warning omitted grain/decode/replay/fixed-mode limits.\n";
        PostMessageW(dialog.result, WM_COMMAND, IDCANCEL, 0); return false;
    }
    PostMessageW(dialog.result, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), reinterpret_cast<LPARAM>(yes));
    return waitUntil([&] { return !IsWindow(dialog.result) && controlText(button).find(L"开*") != std::wstring::npos; },
        std::chrono::seconds(5));
}

BOOL CALLBACK findAutomaticDetailDialog(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (processId == context.processId && std::wstring(name) == L"#32770" &&
        controlText(window) == L"实验性自动细节") { context.result = window; return FALSE; }
    return TRUE;
}

bool confirmAutomaticDetail(HDESK desktop, DWORD processId, HWND parent) {
    const HWND button = GetDlgItem(parent, 1037);
    if (!button || !IsWindowEnabled(button) || controlText(button).find(L"关") == std::wstring::npos) return false;
    if (!PostMessageW(parent, WM_COMMAND, MAKEWPARAM(1037, BN_CLICKED), reinterpret_cast<LPARAM>(button))) return false;
    FindWindowContext dialog{processId};
    if (!waitUntil([&] { EnumDesktopWindows(desktop, findAutomaticDetailDialog, reinterpret_cast<LPARAM>(&dialog));
                         return dialog.result != nullptr; }, std::chrono::seconds(10))) return false;
    const HWND yes = GetDlgItem(dialog.result, IDYES);
    if (!yes) { PostMessageW(dialog.result, WM_COMMAND, IDCANCEL, 0); return false; }
    PostMessageW(dialog.result, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), reinterpret_cast<LPARAM>(yes));
    return waitUntil([&] { return !IsWindow(dialog.result) && controlText(button).find(L"开*") != std::wstring::npos; },
                     std::chrono::seconds(5));
}

BOOL CALLBACK findAutomaticCompressionDialog(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (processId == context.processId && std::wstring(name) == L"#32770" &&
        controlText(window) == L"实验性自动压缩修复") {
        context.result = window; return FALSE;
    }
    return TRUE;
}

bool revealQaControl(HWND parent, HWND control) {
    if (!control || !IsWindow(control)) return false;
    SendMessageW(parent, WM_VSCROLL, SB_TOP, 0);
    for (int step = 0; step < 80; ++step) {
        if (IsWindowVisible(control)) return true;
        SendMessageW(parent, WM_VSCROLL, SB_LINEDOWN, 0);
    }
    return IsWindowVisible(control) != FALSE;
}

bool confirmAutomaticCompression(HDESK desktop, DWORD processId, HWND parent) {
    const HWND button = GetDlgItem(parent, 1032), adjustment = GetDlgItem(parent, 1033);
    const HWND reset = GetDlgItem(parent, 1034), strength = GetDlgItem(parent, 1028);
    if (!button || !adjustment || !reset || !strength || !IsWindowEnabled(button) ||
        controlText(button) != L"自动压缩：关" || controlText(adjustment) != L"0" ||
        IsWindowEnabled(adjustment) || IsWindowEnabled(reset) || !IsWindowEnabled(strength)) {
        std::cerr << "Automatic-compression controls do not start in independent default-off/manual mode.\n";
        return false;
    }
    if (!revealQaControl(parent, button) || !PostMessageW(button, BM_CLICK, 0, 0)) return false;
    FindWindowContext dialog{processId};
    if (!waitUntil([&] {
        EnumDesktopWindows(desktop, findAutomaticCompressionDialog, reinterpret_cast<LPARAM>(&dialog));
        return dialog.result != nullptr;
    }, std::chrono::seconds(10))) {
        std::cerr << "Experimental compression explanation did not open in the launched QA process.\n";
        return false;
    }
    std::wstring explanation;
    EnumChildWindows(dialog.result, [](HWND child, LPARAM parameter) -> BOOL {
        auto& text = *reinterpret_cast<std::wstring*>(parameter); text += controlText(child); text += L'\n'; return TRUE;
    }, reinterpret_cast<LPARAM>(&explanation));
    const bool truthful = explanation.find(L"SDR8") != std::wstring::npos && explanation.find(L"8 像素") != std::wstring::npos &&
        explanation.find(L"纹理可能误判") != std::wstring::npos && explanation.find(L"低置信度") != std::wstring::npos &&
        explanation.find(L"软件解码") != std::wstring::npos && explanation.find(L"重放") != std::wstring::npos &&
        explanation.find(L"默认关闭") != std::wstring::npos && explanation.find(L"不是细节增强") != std::wstring::npos;
    const HWND yes = GetDlgItem(dialog.result, IDYES);
    if (!truthful || !yes) {
        std::cerr << "Compression warning omitted analyzer/texture/confidence/decode/replay/detail limitations.\n";
        PostMessageW(dialog.result, WM_COMMAND, IDCANCEL, 0); return false;
    }
    PostMessageW(dialog.result, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), reinterpret_cast<LPARAM>(yes));
    if (!waitUntil([&] {
        return !IsWindow(dialog.result) && controlText(button) == L"自动压缩：开*" &&
            IsWindowEnabled(adjustment) && IsWindowEnabled(reset) && !IsWindowEnabled(strength);
    }, std::chrono::seconds(5))) return false;
    if (!revealQaControl(parent, adjustment) || !IsWindowVisible(reset)) {
        std::cerr << "Automatic-compression bias/reset cannot be scrolled into the real GUI viewport.\n";
        return false;
    }
    const auto setAdjustment = [&](const wchar_t* value) {
        if (!SendMessageW(adjustment, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value))) return false;
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(1033, EN_CHANGE), reinterpret_cast<LPARAM>(adjustment));
        return controlText(adjustment) == value && !IsWindowEnabled(strength);
    };
    if (!setAdjustment(L"-25.5")) return false;
    SendMessageW(reset, BM_CLICK, 0, 0);
    if (!waitUntil([&] { return controlText(adjustment) == L"0"; }, std::chrono::seconds(5)) || !setAdjustment(L"-25.5")) {
        std::cerr << "Automatic-compression signed adjustment/reset did not preserve the explicit bias.\n";
        return false;
    }
    std::cout << "Actual EXE warning accepted; independent compression bias -25.5%, reset to 0 and restored -25.5%; manual strength is disabled.\n";
    return true;
}

bool freshPortableQa(const std::filesystem::path& executable) {
    // A separate desktop does NOT isolate LocalAppData. Never enable this new
    // GPU test against an installed/raw EXE or an existing portable queue.
    const auto root = executable.parent_path();
    for (auto ancestor = root; !ancestor.empty();) {
        const auto attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = parent;
    }
    if (GetDriveTypeW(root.root_path().c_str()) == DRIVE_REMOTE) return false;
    std::ifstream marker(root / L"portable-mode.txt", std::ios::binary);
    std::string line; std::getline(marker, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
    if (line != "RifeLovePortable 1") return false;
    const auto data = root / L"data";
    const auto attributes = GetFileAttributesW(data.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
        if (!CreateDirectoryW(data.c_str(), nullptr)) return false;
    } else {
        std::error_code error;
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !std::filesystem::is_empty(data, error) || error) return false;
    }
    const auto probe = data / (L".native-exe-test-probe-" + std::to_wstring(GetCurrentProcessId()));
    const HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char byte = '1'; DWORD written = 0;
    const bool writable = WriteFile(file, &byte, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file);
    CloseHandle(file);
    return writable;
}

bool verifyQaQueue(const std::filesystem::path& database, const std::filesystem::path& source,
    const std::filesystem::path& output, bool automaticDenoise, bool compression = false, bool compressionTiled = false,
    bool automaticCompression = false, bool automaticDetailSr = false) {
    // Use the Windows SQLite API read-only; no new link/runtime dependency is
    // added to the product or to this standalone User32 test driver.
    HMODULE module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { std::cerr << "Windows SQLite is unavailable for read-only QA verification.\n"; return false; }
    const auto unload = [](HINSTANCE__* value) { if (value) FreeLibrary(value); };
    std::unique_ptr<HINSTANCE__, decltype(unload)> library(module, unload);
    using Open = int (*)(const char*, void**, int, const char*);
    using Close = int (*)(void*);
    using Prepare = int (*)(void*, const char*, int, void**, const char**);
    using Bind = int (*)(void*, int, const char*, int, void (*)(void*));
    using Step = int (*)(void*);
    using Column = long long (*)(void*, int);
    using Finalize = int (*)(void*);
    using Error = const char* (*)(void*);
    const auto open = reinterpret_cast<Open>(GetProcAddress(module, "sqlite3_open_v2"));
    const auto close = reinterpret_cast<Close>(GetProcAddress(module, "sqlite3_close"));
    const auto prepare = reinterpret_cast<Prepare>(GetProcAddress(module, "sqlite3_prepare_v2"));
    const auto bind = reinterpret_cast<Bind>(GetProcAddress(module, "sqlite3_bind_text"));
    const auto step = reinterpret_cast<Step>(GetProcAddress(module, "sqlite3_step"));
    const auto column = reinterpret_cast<Column>(GetProcAddress(module, "sqlite3_column_int64"));
    const auto finalize = reinterpret_cast<Finalize>(GetProcAddress(module, "sqlite3_finalize"));
    const auto error = reinterpret_cast<Error>(GetProcAddress(module, "sqlite3_errmsg"));
    if (!open || !close || !prepare || !bind || !step || !column || !finalize || !error) return false;
    void* raw = nullptr; const auto path = utf8(database.c_str());
    const int status = open(path.c_str(), &raw, 1 /* SQLITE_OPEN_READONLY */, nullptr);
    std::unique_ptr<void, Close> db(raw, close);
    if (status != 0) { std::cerr << "Could not open the QA queue read-only.\n"; return false; }
    std::string sql = compression ?
        "SELECT COUNT(*), MAX(json_extract(d.result,'$.automaticCompressionRestoration.analyzedSourceFrames')), "
        "MAX(json_extract(d.result,'$.automaticCompressionRestoration.confidentSourceFrames')), "
        "MAX(json_extract(d.result,'$.automaticCompressionRestoration.conditionedSourceFrames')) "
        "FROM tasks t JOIN task_details d ON d.task_id=t.id "
        "WHERE t.source_path=?1 AND t.output_path=?2 AND t.state='completed' "
        "AND json_extract(d.configuration,'$.schemaVersion') IN (10,11,12,13,14) "
        "AND json_extract(d.configuration,'$.targetRate')='60/1' "
        "AND json_extract(d.configuration,'$.compressionRestorationModelDirectory')<>'' "
        "AND length(json_extract(d.configuration,'$.expectedCompressionRestorationDescriptorSha256'))=64 "
        "AND json_extract(d.configuration,'$.compressionRestorationStrength')=0.5 "
        "AND json_extract(d.configuration,'$.temporalRestorationModelDirectory')='' "
        "AND json_extract(d.configuration,'$.restorationModelDirectory')='' "
        "AND json_extract(d.result,'$.sourceDecodeAcceleration')='software' "
        "AND json_extract(d.result,'$.backend')<>'' AND json_extract(d.result,'$.encoder')='h264_nvenc' "
        "AND json_extract(d.result,'$.sourceFrames')>1 AND json_extract(d.result,'$.inferenceCalls')>0 "
        "AND json_extract(d.result,'$.outputFrames')>1 "
        "AND json_extract(d.result,'$.validation.decodedFrames')=json_extract(d.result,'$.outputFrames')" : automaticDenoise ?
        "SELECT COUNT(*) FROM tasks t JOIN task_details d ON d.task_id=t.id "
        "WHERE t.source_path=?1 AND t.output_path=?2 AND t.state='completed' "
        "AND json_extract(d.configuration,'$.schemaVersion') IN (10,11,12,13,14) "
        "AND json_extract(d.configuration,'$.temporalRestorationModelDirectory')<>'' "
        "AND json_extract(d.configuration,'$.automaticTemporalDenoise.algorithmVersion')=2 "
        "AND json_extract(d.configuration,'$.automaticTemporalDenoise.replayPolicy')='software-source-pts-from-start-v1' "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.active')=1 "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.algorithmVersion')=2 "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.analyzedSourceFrames')>0 "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.countsScope')='current_execution' "
        "AND json_extract(d.result,'$.sourceDecodeAcceleration')='software' "
        "AND json_extract(d.result,'$.temporalRestorationBackend')<>'' "
        "AND json_extract(d.result,'$.outputFrames')>0 "
        "AND json_extract(d.result,'$.validation.decodedFrames')=json_extract(d.result,'$.outputFrames')" :
        "SELECT COUNT(*) FROM tasks t JOIN task_details d ON d.task_id=t.id "
        "WHERE t.source_path=?1 AND t.output_path=?2 AND t.state='completed' "
        "AND json_extract(d.configuration,'$.targetRate')='60/1' "
        "AND json_extract(d.result,'$.modelId')<>'' AND json_extract(d.result,'$.backend')<>'' "
        "AND json_extract(d.result,'$.encoder')='h264_nvenc' "
        "AND json_extract(d.result,'$.sourceFrames')>1 AND json_extract(d.result,'$.aiFrames')>0 "
        "AND json_extract(d.result,'$.inferenceCalls')>0 AND json_extract(d.result,'$.outputFrames')>1 "
        "AND json_extract(d.result,'$.validation.decodedFrames')=json_extract(d.result,'$.outputFrames')";
    // SQLITE_STATIC bindings stay alive until statement finalization.
    if (compression) sql += compressionTiled ?
        " AND json_extract(d.configuration,'$.schemaVersion') IN (11,12,13,14) AND json_extract(d.configuration,'$.compressionRestorationTiled')=1" :
        " AND COALESCE(json_extract(d.configuration,'$.compressionRestorationTiled'),0)=0";
    if (automaticCompression) {
        sql +=
            " AND json_extract(d.configuration,'$.schemaVersion') IN (12,13,14) "
            "AND json_type(d.configuration,'$.automaticCompressionRestoration')='object' "
            "AND (SELECT COUNT(*) FROM json_each(json_extract(d.configuration,'$.automaticCompressionRestoration')))=7 "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.algorithmVersion')=2 "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.analyzer')='sdr8-luma-block8-stratified-v2' "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.replayPolicy')='software-source-pts-from-start-v1' "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.relativeAdjustment')=-0.255 "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.smoothingSeconds')=0.35 "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.maximumStrengthChangePerSecond')=0.5 "
            "AND json_extract(d.configuration,'$.automaticCompressionRestoration.minimumConfidence')=0.6 "
            "AND json_type(d.configuration,'$.automaticTemporalDenoise')='null' "
            "AND json_extract(d.result,'$.compressionRestorationModelId')<>'' "
            "AND json_extract(d.result,'$.compressionRestorationBackend')<>'' "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.active')=1 "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.algorithmVersion')=2 "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.analyzer')='sdr8-luma-block8-stratified-v2' "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.countsScope')='current_execution_including_replay' "
            "AND json_type(d.result,'$.automaticCompressionRestoration.analyzedSourceFrames')='integer' "
            "AND json_type(d.result,'$.automaticCompressionRestoration.confidentSourceFrames')='integer' "
            "AND json_type(d.result,'$.automaticCompressionRestoration.conditionedSourceFrames')='integer' "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.analyzedSourceFrames')=json_extract(d.result,'$.sourceFrames') "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.confidentSourceFrames') BETWEEN 0 AND json_extract(d.result,'$.automaticCompressionRestoration.analyzedSourceFrames') "
            "AND json_extract(d.result,'$.automaticCompressionRestoration.conditionedSourceFrames') BETWEEN 0 AND json_extract(d.result,'$.automaticCompressionRestoration.confidentSourceFrames') "
            "AND json_extract(d.result,'$.automaticGaussianDenoise.active')=0";
        sql += compressionTiled ?
            " AND json_extract(d.configuration,'$.compressionRestorationPolicy.mode')='tiled' "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.tilePolicy')='stdf-r3-core256x128-halo64-global-edge-pad8-eval-v1' "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.coreWidth')=256 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.coreHeight')=128 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.halo')=64 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.alignment')=8 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumInputWidth')=384 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumInputHeight')=256 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumAxis')=3840 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumPixels')=8294400 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.edgePolicy')='global-right-bottom-pad-to8-clip-halo-v1'" :
            " AND json_extract(d.configuration,'$.compressionRestorationPolicy.mode')='full-frame' "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.tilePolicy')='full-frame-bounded-384x256-v1' "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumInputWidth')=384 "
            "AND json_extract(d.configuration,'$.compressionRestorationPolicy.maximumInputHeight')=256";
    } else if (compression) {
        sql += " AND json_extract(d.configuration,'$.automaticCompressionRestoration') IS NULL "
            "AND COALESCE(json_extract(d.result,'$.automaticCompressionRestoration.active'),0)=0";
    }
    if (automaticDetailSr) {
        sql += " AND json_extract(d.configuration,'$.schemaVersion')=14 "
            "AND json_extract(d.configuration,'$.deblurModelDirectory')<>'' "
            "AND length(json_extract(d.configuration,'$.expectedDeblurDescriptorSha256'))=64 "
            "AND json_extract(d.configuration,'$.restorationModelDirectory')<>'' "
            "AND length(json_extract(d.configuration,'$.expectedRestorationDescriptorSha256'))=64 "
            "AND json_extract(d.configuration,'$.outputImageOrder')='deblur-before-super-resolution-v1' "
            "AND json_extract(d.configuration,'$.automaticDetailRestoration.algorithmVersion')=2 "
            "AND json_extract(d.result,'$.automaticDetailRestoration.active')=1 "
            "AND json_extract(d.result,'$.automaticDetailRestoration.conditionedOutputFrames')>0 "
            "AND json_extract(d.result,'$.deblurRestorationModelId')='nafnet-zoo-gopro-sdr8-local' "
            "AND json_extract(d.result,'$.restorationModelId')='realesrgan-x2plus-sdr8-local' "
            "AND json_extract(d.result,'$.sourceDecodeAcceleration')='software'";
    }
    const auto input = utf8(source.c_str()), destination = utf8(output.c_str());
    void* statementRaw = nullptr;
    const int prepared = prepare(raw, sql.c_str(), -1, &statementRaw, nullptr);
    std::unique_ptr<void, Finalize> statement(statementRaw, finalize);
    if (prepared != 0) { std::cerr << "Read-only QA query needs SQLite JSON support: " << error(raw) << '\n'; return false; }
    if (bind(statementRaw, 1, input.c_str(), -1, nullptr) != 0 || bind(statementRaw, 2, destination.c_str(), -1, nullptr) != 0 ||
        step(statementRaw) != 100 /* SQLITE_ROW */ || column(statementRaw, 0) != 1) {
        std::cerr << "Completed QA queue record did not prove the selected processing mode/result.\n"; return false;
    }
    if (automaticCompression) {
        const auto analyzed = column(statementRaw, 1), confident = column(statementRaw, 2), applied = column(statementRaw, 3);
        std::cout << "Read-only schema12 QA queue proves the complete automatic-compression analyzer/replay/settings and explicit "
            << (compressionTiled ? "tiled" : "full-frame") << " policy; source counters: analyzed=" << analyzed
            << ", confident=" << confident << ", STDF-conditioned=" << applied << ".\n";
        if (!applied) std::cout << "Every source frame bypassed STDF under the conservative confidence policy; this run proves analysis/control flow, not restoration inference or visual-quality qualification.\n";
    }
    std::cout << (compression ? "QA queue confirms independent STDF opt-in at 50%, explicit full-frame/tiled policy, pinned descriptor, software decode, real RIFE calls and matching validated completed frame counts. This is functional pipeline evidence, not visual-quality qualification.\n" :
        automaticDenoise ? "QA queue confirms automatic opt-in, selected temporal model, software decode and validated completed frame counts.\n" :
        "QA queue confirms this source/output completed at 60 FPS with a selected model/backend, real AI calls, H.264 NVENC and matching validated frame counts.\n");
    return true;
}

bool samePath(const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto a = first.lexically_normal().generic_wstring(), b = second.lexically_normal().generic_wstring();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool comparisonQaFixtures(const std::filesystem::path& executable, const std::filesystem::path& source,
    const std::filesystem::path& output) {
    const auto root = executable.parent_path(), fixtures = root / L"qa-fixtures";
    // The marker is the caller's explicit synthetic-fixture declaration. It
    // does not prove visual quality or permit reading arbitrary user videos.
    if (executable.filename() != L"RifeLoveNative.exe" || !samePath(source.parent_path(), fixtures) ||
        !samePath(output.parent_path(), fixtures) || source.filename() != L"synthetic-comparison-source.mp4" ||
        output.filename() != L"synthetic-comparison-output.mkv") return false;
    if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES ||
        (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)) return false;
    for (const auto& file : {fixtures, source, fixtures / L"synthetic-fixtures.txt", root / L"PACKAGE-MANIFEST.json", root / L"SHA256SUMS.txt"}) {
        const auto attributes = GetFileAttributesW(file.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (file != fixtures && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    std::ifstream marker(fixtures / L"synthetic-fixtures.txt", std::ios::binary);
    std::string line; std::getline(marker, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::error_code error;
    const auto bytes = std::filesystem::file_size(source, error);
    return line == "RifeLoveSyntheticComparison 1" && !error && bytes > 0 && bytes <= 16U * 1024U * 1024U;
}

std::optional<std::wstring> boundedControlText(HWND window) {
    if (!window || !IsWindow(window)) return {};
    wchar_t value[4096]{}; DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(window, WM_GETTEXT, static_cast<WPARAM>(std::size(value)), reinterpret_cast<LPARAM>(value),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &length) || length >= std::size(value) - 1) return {};
    return std::wstring(value);
}

struct ComparisonPosition {
    std::int64_t numerator = 0, denominator = 1;
    long double seconds() const { return static_cast<long double>(numerator) / denominator; }
    bool operator==(const ComparisonPosition&) const = default;
};

std::optional<ComparisonPosition> comparisonPosition(HWND comparison) {
    const auto text = boundedControlText(GetDlgItem(comparison, 1307));
    if (!text || text->empty()) return {};
    wchar_t* end = nullptr; errno = 0;
    const auto numerator = std::wcstoll(text->c_str(), &end, 10);
    if (errno || end == text->c_str() || numerator < 0 || numerator > 1000000000000LL) return {};
    long long denominator = 1;
    if (*end == L'/') {
        const auto* begin = end + 1; errno = 0;
        denominator = std::wcstoll(begin, &end, 10);
        if (errno || end == begin || denominator <= 0 || denominator > 1000000000LL) return {};
    }
    if (*end) return {};
    const auto divisor = std::gcd(numerator, denominator);
    return ComparisonPosition{numerator / divisor, denominator / divisor};
}

bool comparisonAction(HWND comparison, int id) {
    const HWND button = GetDlgItem(comparison, id); DWORD_PTR result = 0;
    return button && IsWindowEnabled(button) && SendMessageTimeoutW(button, BM_CLICK, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
}

bool comparisonPaused(HWND comparison) {
    const auto button = boundedControlText(GetDlgItem(comparison, 1312));
    const auto status = boundedControlText(GetDlgItem(comparison, 1310));
    return button && *button == L"播放 / 静音" && IsWindowEnabled(GetDlgItem(comparison, 1312)) &&
        status && status->starts_with(L"已暂停 · 静音 · 请求 ") && comparisonPosition(comparison).has_value();
}

bool checkPackagedComparison(HDESK desktop, DWORD processId, HWND owner,
    const std::filesystem::path& source, const std::filesystem::path& output) {
    const auto fail = [](const char* reason) { std::cerr << "Packaged comparison failed: " << reason << '\n'; return false; };
    const HWND action = GetDlgItem(owner, 1025); DWORD_PTR ignored = 0;
    if (!action || !IsWindowEnabled(action) || !SendMessageTimeoutW(action, BM_CLICK, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &ignored)) return fail("main comparison action unavailable");
    FindWindowContext context{processId, nullptr, L"RifeLoveNativeComparisonWindow"};
    if (!waitUntil([&] { EnumDesktopWindows(desktop, findTargetWindow, reinterpret_cast<LPARAM>(&context)); return context.result != nullptr; },
        std::chrono::seconds(10))) return fail("no comparison window in the launched package process");
    const HWND comparison = context.result;
    if (!GetDlgItem(comparison, 1312)) return fail("packaged executable does not contain the synchronized-playback control");
    if (!waitUntil([&] { return comparisonPaused(comparison); }, std::chrono::seconds(30))) return fail("initial decoded pair never became ready");
    const auto shownSource = boundedControlText(GetDlgItem(comparison, 1301)), shownOutput = boundedControlText(GetDlgItem(comparison, 1302));
    const auto initial = comparisonPosition(comparison);
    if (!shownSource || !shownOutput || !samePath(std::filesystem::path(*shownSource), source) ||
        !samePath(std::filesystem::path(*shownOutput), output) || !initial || initial->numerator != 0)
        return fail("loaded file paths or initial timestamp do not match this fresh QA task");
    const auto explanation = boundedControlText(GetDlgItem(comparison, 1310));
    if (!explanation || explanation->find(L"原片 ") == std::wstring::npos || explanation->find(L"结果 ") == std::wstring::npos ||
        explanation->find(L"30 FPS") == std::wstring::npos) return fail("paired decode timestamps or preview limit are not observable");
    if (!comparisonAction(comparison, 1312)) return fail("could not click Play");
    ComparisonPosition first, second;
    const auto advanced = [&](long double minimum, ComparisonPosition& observed) {
        return waitUntil([&] {
            const auto label = boundedControlText(GetDlgItem(comparison, 1312));
            const auto status = boundedControlText(GetDlgItem(comparison, 1310));
            const auto position = comparisonPosition(comparison);
            if (!label || *label != L"暂停" || !status || !status->starts_with(L"同步播放 · 静音 · 请求 ") ||
                !position || position->seconds() <= minimum) return false;
            observed = *position; return true;
        }, std::chrono::seconds(10));
    };
    if (!advanced(0.05L, first) || !advanced(first.seconds() + 0.04L, second)) return fail("successive decoded output timestamps did not advance while playing");
    const auto secondPlayback = second;
    if (!comparisonAction(comparison, 1312) || !waitUntil([&] { return comparisonPaused(comparison); }, std::chrono::seconds(5)))
        return fail("Pause did not settle");
    const auto frozen = comparisonPosition(comparison);
    if (!frozen) return fail("paused presentation time is absent");
    const auto frozenUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    do {
        const auto position = comparisonPosition(comparison);
        if (!comparisonPaused(comparison) || !position || *position != *frozen) return fail("a late decode changed the paused presentation time");
        Sleep(20);
    } while (std::chrono::steady_clock::now() < frozenUntil);
    if (!comparisonAction(comparison, 1306) || !waitUntil([&] {
        const auto position = comparisonPosition(comparison);
        return comparisonPaused(comparison) && position && position->seconds() > frozen->seconds();
    }, std::chrono::seconds(10))) return fail("next-frame decode did not advance");
    const auto next = comparisonPosition(comparison);
    // The output container may quantize a nominal 60 FPS stream to a millisecond
    // time base (0, 17, 33, 50 ms ...). Comparing against 1/60 exactly would
    // reject a correct adjacent frame. The product exposes the actual adjacent
    // PTS through its next-frame action; prove adjacency by selecting the next
    // frame, then requiring the previous action to return to the exact frozen
    // rational position below. The read-only queue check separately proves that
    // this QA job requested 60 FPS.
    if (!next || next->seconds() <= frozen->seconds())
        return fail("next-frame action did not select a later output PTS");
    if (!comparisonAction(comparison, 1305) || !waitUntil([&] {
        const auto position = comparisonPosition(comparison); return comparisonPaused(comparison) && position && *position == *frozen;
    }, std::chrono::seconds(10))) return fail("previous-frame action did not return to the exact paused PTS");
    if (!SendMessageTimeoutW(GetDlgItem(comparison, 1307), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"1/2"),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored) || !comparisonAction(comparison, 1308) || !waitUntil([&] {
        const auto position = comparisonPosition(comparison);
        return comparisonPaused(comparison) && position && *position == ComparisonPosition{1, 2};
    }, std::chrono::seconds(10))) return fail("exact half-second seek did not decode the requested pair");
    if (!comparisonAction(comparison, 1312) || !advanced(0.55L, second) || !comparisonAction(comparison, 1312) ||
        !waitUntil([&] { return comparisonPaused(comparison); }, std::chrono::seconds(5))) return fail("playback did not resume and pause after exact seeking");
    if (!PostMessageW(comparison, WM_CLOSE, 0, 0) || !waitUntil([&] { return !IsWindow(comparison); }, std::chrono::seconds(5)) || !IsWindow(owner))
        return fail("comparison close did not preserve the main package window");
    std::cout << "Actual packaged EXE comparison reports this source/result decoded, successive playback PTS " << first.numerator << '/' << first.denominator <<
        " then " << secondPlayback.numerator << '/' << secondPlayback.denominator << ", frozen frame PTS " << frozen->numerator << '/' << frozen->denominator <<
        ", next frame PTS " << next->numerator << '/' << next->denominator << ", a fixed 300 ms pause, actual adjacent 60 FPS-job forward/backward stepping and seek/resume.\n"
        "Evidence is cross-process child-control state/PTS and a separate read-only queue check; no screenshot, framebuffer rendering or visual-quality claim is made.\n";
    return true;
}
} // namespace

// Developer-only end-to-end validation. The target runs on a separate desktop
// that is never switched to, so no user input or foreground window is touched.
int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Expected native executable, source fixture, new output path and optional screenshot, --blur, --300, --360, --480, --600, --denoise, --auto-denoise, --dual, --dual-blur, --compression, --compression-tiled, --auto-compression, --auto-compression-tiled or --comparison; or native executable --capture-only new-client.bmp. All compression modes, --auto-denoise, --comparison and --capture-only require a fresh writable portable QA package. Full-frame compression requires a tagged BT.709 limited-range SDR8 yuv420p source no larger than 384x256 and a qualified local STDF pack. The -tiled variants explicitly enable the separate bounded UHD tile evaluation path. Automatic compression accepts its experimental warning, tests -25.5%/reset and validates schema12 analyzer/replay/settings and result counters read-only. --comparison additionally requires qa-fixtures/synthetic-fixtures.txt declaring RifeLoveSyntheticComparison 1, synthetic-comparison-source.mp4 and new synthetic-comparison-output.mkv inside that package.\n";
        return 2;
    }
    const std::filesystem::path executable = std::filesystem::absolute(argv[1]).lexically_normal();
    const bool captureOnly = argc == 4 && std::wstring(argv[2]) == L"--capture-only";
    const std::filesystem::path source = captureOnly ? std::filesystem::path{} : std::filesystem::absolute(argv[2]).lexically_normal();
    const std::filesystem::path output = std::filesystem::absolute(argv[3]).lexically_normal();
    const bool automaticDenoise = argc == 5 && std::wcscmp(argv[4], L"--auto-denoise") == 0;
    const bool automaticDetailSr = argc == 5 && std::wcscmp(argv[4], L"--auto-detail-sr") == 0;
    const bool automaticCompression = argc == 5 &&
        (std::wcscmp(argv[4], L"--auto-compression") == 0 || std::wcscmp(argv[4], L"--auto-compression-tiled") == 0);
    const bool compressionTiled = argc == 5 &&
        (std::wcscmp(argv[4], L"--compression-tiled") == 0 || std::wcscmp(argv[4], L"--auto-compression-tiled") == 0);
    const bool compression = automaticCompression || compressionTiled || (argc == 5 && std::wcscmp(argv[4], L"--compression") == 0);
    const bool comparison = argc == 5 && std::wcscmp(argv[4], L"--comparison") == 0;
    if (!std::filesystem::is_regular_file(executable) || (!captureOnly && !std::filesystem::is_regular_file(source)) ||
        std::filesystem::exists(output)) {
        std::cerr << "Executable/fixture is missing or output already exists.\n";
        return 2;
    }
    if (comparison && !comparisonQaFixtures(executable, source, output)) {
        std::cerr << "Comparison QA requires a packaged RifeLoveNative.exe with manifest/checksums and declared small synthetic fixtures inside its qa-fixtures directory; arbitrary videos, links and existing outputs are refused before launch.\n";
        return 2;
    }
    if ((automaticDenoise || automaticDetailSr || compression || captureOnly || comparison) && !freshPortableQa(executable)) {
        std::cerr << "This EXE QA mode requires a local portable-mode.txt and absent/empty writable data directory; existing queues are never used.\n";
        return 2;
    }
    const std::wstring name = L"RifeLoveNativeExeTest_" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) {
        std::cerr << "Cannot create isolated desktop: " << GetLastError() << '\n';
        return 1;
    }
    std::wstring desktopPath = L"WinSta0\\" + name;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktopPath.data();
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        executable.parent_path().c_str(), &startup, &process)) {
        std::cerr << "Cannot start target executable: " << GetLastError() << '\n';
        CloseDesktop(desktop);
        return 1;
    }
    CloseHandle(process.hThread);
    // Wait for WM_CREATE and resource discovery to finish before sending UI
    // messages; an HWND can exist before its child controls are ready.
    WaitForInputIdle(process.hProcess, 30000);
    int result = 1;
    FindWindowContext context{process.dwProcessId};
    const bool found = waitUntil([&] {
        EnumDesktopWindows(desktop, findTargetWindow, reinterpret_cast<LPARAM>(&context));
        return context.result != nullptr || WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0;
    }, std::chrono::seconds(30));
    if (found && context.result) {
        if (captureOnly) {
            const bool controlsReady = waitUntil([&] { return GetDlgItem(context.result, 1009) != nullptr; }, std::chrono::seconds(10));
            if (controlsReady && captureTarget(context.result, output)) {
                std::cout << "Actual EXE client-area BMP composite captured on its private desktop without starting a video task.\n";
                result = 0;
            } else std::cerr << "Could not capture the actual EXE client background and controls.\n";
        } else {
        const std::wstring testOption = argc == 5 ? std::wstring(argv[4]) : std::wstring{};
        if (!testOption.empty() && !testOption.starts_with(L"--") &&
            !captureTarget(context.result, std::filesystem::absolute(argv[4])))
            std::cerr << "Could not capture the target window before checking readiness.\n";
        waitUntil([&] {
            return GetDlgItem(context.result, 1001) && GetDlgItem(context.result, 1003) &&
                   GetDlgItem(context.result, 1006) && GetDlgItem(context.result, 1009) &&
                   GetDlgItem(context.result, 1010);
        }, std::chrono::seconds(10));
        const HWND sourceEdit = GetDlgItem(context.result, 1001), outputEdit = GetDlgItem(context.result, 1003);
        const HWND encoder = GetDlgItem(context.result, 1006), start = GetDlgItem(context.result, 1009);
        const HWND cancel = GetDlgItem(context.result, 1010);
        if (sourceEdit && outputEdit && encoder && start && cancel) {
            SendMessageW(encoder, CB_SETCURSEL, 1, 0); // Explicit H.264 NVENC.
            SendMessageW(context.result, WM_COMMAND, MAKEWPARAM(1006, CBN_SELCHANGE), reinterpret_cast<LPARAM>(encoder));
            // SetWindowTextW does not update another process's child edit
            // controls. WM_SETTEXT is a system message and is marshalled by
            // User32 between these two processes on the private desktop.
            const LRESULT sourceSet = SendMessageW(sourceEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(source.c_str()));
            const LRESULT outputSet = SendMessageW(outputEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(output.c_str()));
            if (!sourceSet || !outputSet) std::cerr << "Could not set source/output in target controls.\n";
            // WM_SETTEXT is marshalled across processes, but an externally
            // sent edit update does not reliably deliver the child EN_CHANGE
            // notification to its owning window. Reproduce the notification
            // that normal user typing emits so the application inspects the
            // source and adopts the requested output path before readiness is
            // checked.
            SendMessageW(context.result, WM_COMMAND, MAKEWPARAM(1001, EN_CHANGE), 0);
            SendMessageW(context.result, WM_COMMAND, MAKEWPARAM(1003, EN_CHANGE), 0);
            if (waitUntil([&] { return IsWindowEnabled(start) != FALSE; }, std::chrono::seconds(30))) {
                const std::wstring option = argc == 5 ? std::wstring(argv[4]) : std::wstring{};
                const bool blur = option == L"--blur" || option == L"--dual-blur";
                const bool dual = option == L"--dual" || option == L"--dual-blur";
                const bool denoise = argc == 5 && (std::wstring(argv[4]) == L"--denoise" || dual || automaticDenoise);
                const bool highRate = argc == 5 && (std::wstring(argv[4]) == L"--300" || std::wstring(argv[4]) == L"--360" || std::wstring(argv[4]) == L"--480" || std::wstring(argv[4]) == L"--600");
                if (automaticDetailSr) {
                    const bool ready = selectComboItemByLabel(context.result, 1005, L"60 FPS") &&
                        selectComboItemByLabel(context.result, 1036, L"NAFNet") &&
                        selectComboItemByLabel(context.result, 1017, L"超分辨率") &&
                        confirmAutomaticDetail(desktop, process.dwProcessId, context.result) && IsWindowEnabled(start);
                    if (!ready) {
                        std::cerr << "Independent deblur, super-resolution or automatic detail controls were unavailable.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
                            TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
                        }
                        CloseHandle(process.hProcess); CloseDesktop(desktop); return 1;
                    }
                }
                if ((comparison || compression) && !selectComboItemByLabel(context.result, 1005, L"60 FPS")) {
                    std::cerr << "Comparison/compression QA requires an explicitly selected 60 FPS output.\n";
                    PostMessageW(context.result, WM_CLOSE, 0, 0);
                    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
                        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
                    }
                    CloseHandle(process.hProcess); CloseDesktop(desktop); return 1;
                }
                if (denoise) {
                    const HWND temporal = GetDlgItem(context.result, 1024);
                    if (!temporal || !selectComboItemByLabel(context.result, 1024, L"FastDVDnet")) {
                        std::cerr << "Optional denoise model is unavailable.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                    if (!IsWindowEnabled(start)) {
                        std::cerr << "Denoise model is not ready for the selected source.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                }
                if (dual) {
                    const HWND spatial = GetDlgItem(context.result, 1017);
                    if (!spatial || !selectComboItemByLabel(context.result, 1017, L"超分辨率")) {
                        std::cerr << "Optional super-resolution model is unavailable.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                    if (!IsWindowEnabled(start)) {
                        std::cerr << "Dual restoration models are not ready for the selected source.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                }
                if (compression) {
                    const HWND strength = GetDlgItem(context.result, 1028);
                    const HWND tiling = GetDlgItem(context.result, 1029);
                    const bool defaultOff = strength && tiling && SendMessageW(strength, CB_GETCURSEL, 0, 0) == 0 &&
                        SendMessageW(tiling, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
                    // Select two independent HWND controls; denoise/SR remain
                    // off. The app owns all descriptor and source gates.
                    bool controlsReady = defaultOff && selectComboItemByLabel(context.result, 1027, L"STDF") &&
                        IsWindowEnabled(strength) && selectComboItemByLabel(context.result, 1028, L"50%");
                    if (controlsReady && compressionTiled) {
                        controlsReady = IsWindowEnabled(tiling) != FALSE;
                        if (controlsReady) SendMessageW(tiling, BM_CLICK, 0, 0);
                        controlsReady = controlsReady && SendMessageW(tiling, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    }
                    if (controlsReady && automaticCompression)
                        controlsReady = confirmAutomaticCompression(desktop, process.dwProcessId, context.result);
                    if (!controlsReady || !IsWindowEnabled(start)) {
                        std::cerr << "Independent STDF model/strength/tile controls are unavailable or source readiness failed.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
                            TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
                        }
                        CloseHandle(process.hProcess); CloseDesktop(desktop); return 1;
                    }
                    std::cout << "Actual EXE HWNDs expose independent model/strength/tile controls 1027/1028/1029; selected 50%, "
                        << (compressionTiled ? "explicit tiled mode" : "original full-frame mode")
                        << (automaticCompression ? ", explicit automatic bias -25.5%" : ", manual strength") << " and source readiness passed.\n";
                }
                if (automaticDenoise && !confirmAutomaticDenoise(desktop, process.dwProcessId, context.result)) {
                    std::cerr << "Could not explicitly enable experimental automatic Gaussian denoise.\n";
                    PostMessageW(context.result, WM_CLOSE, 0, 0);
                    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
                        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
                    }
                    CloseHandle(process.hProcess); CloseDesktop(desktop); return 1;
                }
                if (highRate) {
                    const HWND rate = GetDlgItem(context.result, 1005);
                    const std::wstring rateLabel = std::wstring(argv[4]) == L"--300" ? L"300 FPS" :
                        std::wstring(argv[4]) == L"--360" ? L"360 FPS" :
                        std::wstring(argv[4]) == L"--480" ? L"480 FPS" : L"600 FPS";
                    if (!rate || !selectComboItemByLabel(context.result, 1005, rateLabel.c_str())) {
                        std::cerr << "Requested high FPS option is unavailable.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                    SendMessageW(context.result, WM_COMMAND, MAKEWPARAM(1005, CBN_SELCHANGE), reinterpret_cast<LPARAM>(rate));
                }
                if (blur) {
                    const HWND checkbox = GetDlgItem(context.result, 1013);
                    // Source inspection and queue recovery can finish one UI
                    // event before updateButtonState re-enables advanced
                    // controls. Wait for the actual control state instead of
                    // treating that transient state as an unavailable
                    // feature.
                    const bool blurReady = checkbox && waitUntil([&] {
                        return IsWindowEnabled(checkbox) != FALSE && IsWindowEnabled(start) != FALSE;
                    }, std::chrono::seconds(5));
                    if (!blurReady) {
                        std::cerr << "Motion blur control is unavailable.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                    SendMessageW(checkbox, BM_CLICK, 0, 0);
                    if (SendMessageW(checkbox, BM_GETCHECK, 0, 0) != BST_CHECKED) {
                        std::cerr << "Motion blur control did not become checked.\n";
                        PostMessageW(context.result, WM_CLOSE, 0, 0);
                        WaitForSingleObject(process.hProcess, 10000);
                        CloseHandle(process.hProcess);
                        CloseDesktop(desktop);
                        return 1;
                    }
                }
                if (argc == 5 && !blur && !highRate && !denoise && !compression && !comparison && !automaticDetailSr && !captureTarget(context.result, std::filesystem::absolute(argv[4])))
                    std::cerr << "Could not capture the target window on the isolated desktop.\n";
                SendMessageW(start, BM_CLICK, 0, 0);
                bool automaticControlsLocked = true;
                if (automaticCompression) {
                    automaticControlsLocked = waitUntil([&] {
                        if (!IsWindowEnabled(cancel)) return false;
                        for (const int id : {1027, 1028, 1029, 1032, 1033, 1034}) {
                            const HWND control = GetDlgItem(context.result, id);
                            if (!control || IsWindowEnabled(control)) return false;
                        }
                        return controlText(GetDlgItem(context.result, 1033)) == L"-25.5";
                    }, std::chrono::seconds(5));
                    if (!automaticControlsLocked) std::cerr << "Actual EXE did not lock compression model/manual/tile/automatic/bias/reset controls during the running queue.\n";
                    else std::cout << "Actual EXE locks all independent compression controls while its queue is active and preserves the captured bias.\n";
                }
                if (waitUntil([&] { return std::filesystem::is_regular_file(output) && !IsWindowEnabled(cancel); },
                              std::chrono::seconds(compressionTiled ? 600 : automaticDenoise || automaticDetailSr || compression ? 300 : 120))) {
                    std::cout << "Actual native EXE published the test video through its start button.\n";
                    result = !automaticControlsLocked ? 1 : comparison ? (checkPackagedComparison(desktop, process.dwProcessId, context.result, source, output) ? 0 : 1) : 0;
                } else std::cerr << "Target did not publish a completed test video.\n";
            } else {
                if (!testOption.empty() && !testOption.starts_with(L"--"))
                    captureTarget(context.result, std::filesystem::absolute(argv[4]));
                std::cerr << "Target did not enable its start button after inspection.\n";
            }
        } else std::cerr << "Target processing controls are missing.\n";
        }
        PostMessageW(context.result, WM_CLOSE, 0, 0);
    } else std::cerr << "Target window did not start on the isolated desktop.\n";

    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        // This is the child created above, on the private test desktop only.
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        result = 1;
    }
    if (result == 0 && (automaticDenoise || automaticDetailSr || compression || comparison) && !verifyQaQueue(executable.parent_path() / L"data" / L"queue.sqlite", source, output, automaticDenoise, compression, compressionTiled, automaticCompression, automaticDetailSr))
        result = 1;
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    return result;
}

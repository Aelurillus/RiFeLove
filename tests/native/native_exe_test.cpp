#include <windows.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace {
struct FindWindowContext { DWORD processId; HWND result = nullptr; };

BOOL CALLBACK findTargetWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<FindWindowContext*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (processId == context.processId && std::wstring(className) == L"RifeLoveNativeDesktopWindow") {
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

bool verifyAutomaticQueue(const std::filesystem::path& database, const std::filesystem::path& source,
    const std::filesystem::path& output) {
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
    constexpr auto sql =
        "SELECT COUNT(*) FROM tasks t JOIN task_details d ON d.task_id=t.id "
        "WHERE t.source_path=?1 AND t.output_path=?2 AND t.state='completed' "
        "AND json_extract(d.configuration,'$.schemaVersion')=9 "
        "AND json_extract(d.configuration,'$.temporalRestorationModelDirectory')<>'' "
        "AND json_extract(d.configuration,'$.automaticTemporalDenoise.algorithmVersion')=1 "
        "AND json_extract(d.configuration,'$.automaticTemporalDenoise.replayPolicy')='software-source-pts-from-start-v1' "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.active')=1 "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.analyzedSourceFrames')>0 "
        "AND json_extract(d.result,'$.automaticGaussianDenoise.countsScope')='current_execution' "
        "AND json_extract(d.result,'$.sourceDecodeAcceleration')='software' "
        "AND json_extract(d.result,'$.temporalRestorationBackend')<>'' "
        "AND json_extract(d.result,'$.outputFrames')>0 "
        "AND json_extract(d.result,'$.validation.decodedFrames')=json_extract(d.result,'$.outputFrames')";
    // SQLITE_STATIC bindings stay alive until statement finalization.
    const auto input = utf8(source.c_str()), destination = utf8(output.c_str());
    void* statementRaw = nullptr;
    const int prepared = prepare(raw, sql, -1, &statementRaw, nullptr);
    std::unique_ptr<void, Finalize> statement(statementRaw, finalize);
    if (prepared != 0) { std::cerr << "Read-only QA query needs SQLite JSON support: " << error(raw) << '\n'; return false; }
    if (bind(statementRaw, 1, input.c_str(), -1, nullptr) != 0 || bind(statementRaw, 2, destination.c_str(), -1, nullptr) != 0 ||
        step(statementRaw) != 100 /* SQLITE_ROW */ || column(statementRaw, 0) != 1) {
        std::cerr << "Completed QA queue record did not prove the selected automatic-denoise mode/result.\n"; return false;
    }
    std::cout << "QA queue confirms schema 9 automatic opt-in, selected temporal model, software decode and validated completed frame counts.\n";
    return true;
}
} // namespace

// Developer-only end-to-end validation. The target runs on a separate desktop
// that is never switched to, so no user input or foreground window is touched.
int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Expected native executable, source fixture, new output path and optional screenshot, --blur, --300, --360, --480, --600, --denoise, --auto-denoise, --dual or --dual-blur; or native executable --capture-only new-client.bmp. --auto-denoise and --capture-only require a fresh writable portable QA package.\n";
        return 2;
    }
    const std::filesystem::path executable = std::filesystem::absolute(argv[1]).lexically_normal();
    const bool captureOnly = argc == 4 && std::wstring(argv[2]) == L"--capture-only";
    const std::filesystem::path source = captureOnly ? std::filesystem::path{} : std::filesystem::absolute(argv[2]).lexically_normal();
    const std::filesystem::path output = std::filesystem::absolute(argv[3]).lexically_normal();
    const bool automaticDenoise = argc == 5 && std::wstring(argv[4]) == L"--auto-denoise";
    if (!std::filesystem::is_regular_file(executable) || (!captureOnly && !std::filesystem::is_regular_file(source)) ||
        std::filesystem::exists(output)) {
        std::cerr << "Executable/fixture is missing or output already exists.\n";
        return 2;
    }
    if ((automaticDenoise || captureOnly) && !freshPortableQa(executable)) {
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
                if (argc == 5 && !blur && !highRate && !denoise && !captureTarget(context.result, std::filesystem::absolute(argv[4])))
                    std::cerr << "Could not capture the target window on the isolated desktop.\n";
                SendMessageW(start, BM_CLICK, 0, 0);
                if (waitUntil([&] { return std::filesystem::is_regular_file(output) && !IsWindowEnabled(cancel); },
                              std::chrono::seconds(automaticDenoise ? 300 : 120))) {
                    std::cout << "Actual native EXE published the test video through its start button.\n";
                    result = 0;
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
    if (result == 0 && automaticDenoise && !verifyAutomaticQueue(executable.parent_path() / L"data" / L"queue.sqlite", source, output))
        result = 1;
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    return result;
}

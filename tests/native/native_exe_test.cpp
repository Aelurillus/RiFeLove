#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
    if (!GetWindowRect(window, &rect)) return false;
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
    const bool painted = PrintWindow(window, dc, 2 /* PW_RENDERFULLCONTENT */) != FALSE;
    std::ofstream stream(output, std::ios::binary);
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
} // namespace

// Developer-only end-to-end validation. The target runs on a separate desktop
// that is never switched to, so no user input or foreground window is touched.
int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Expected native executable, source fixture, new output path and optional screenshot, --blur, --360, --480, --denoise, --dual or --dual-blur.\n";
        return 2;
    }
    const std::filesystem::path executable = std::filesystem::absolute(argv[1]);
    const std::filesystem::path source = std::filesystem::absolute(argv[2]);
    const std::filesystem::path output = std::filesystem::absolute(argv[3]);
    if (!std::filesystem::is_regular_file(executable) || !std::filesystem::is_regular_file(source) ||
        std::filesystem::exists(output)) {
        std::cerr << "Executable/fixture is missing or output already exists.\n";
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
            if (waitUntil([&] { return IsWindowEnabled(start) != FALSE; }, std::chrono::seconds(30))) {
                const std::wstring option = argc == 5 ? std::wstring(argv[4]) : std::wstring{};
                const bool blur = option == L"--blur" || option == L"--dual-blur";
                const bool dual = option == L"--dual" || option == L"--dual-blur";
                const bool denoise = argc == 5 && (std::wstring(argv[4]) == L"--denoise" || dual);
                const bool highRate = argc == 5 && (std::wstring(argv[4]) == L"--360" || std::wstring(argv[4]) == L"--480");
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
                if (highRate) {
                    const HWND rate = GetDlgItem(context.result, 1005);
                    const int selection = std::wstring(argv[4]) == L"--480" ? 6 : 5;
                    if (!rate || SendMessageW(rate, CB_SETCURSEL, selection, 0) == CB_ERR) {
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
                              std::chrono::seconds(120))) {
                    std::cout << "Actual native EXE published the test video through its start button.\n";
                    result = 0;
                } else std::cerr << "Target did not publish a completed test video.\n";
            } else std::cerr << "Target did not enable its start button after inspection.\n";
        } else std::cerr << "Target processing controls are missing.\n";
        PostMessageW(context.result, WM_CLOSE, 0, 0);
    } else std::cerr << "Target window did not start on the isolated desktop.\n";

    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        // This is the child created above, on the private test desktop only.
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        result = 1;
    }
    CloseHandle(process.hProcess);
    CloseDesktop(desktop);
    return result;
}

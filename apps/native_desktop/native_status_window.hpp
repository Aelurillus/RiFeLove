#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace rifelove::native_desktop {

// A selectable snapshot keeps complete diagnostics available even when the
// fixed action dock can show only two lines. Opening it never pauses a job.
class NativeStatusWindow final {
public:
    ~NativeStatusWindow() {
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        if (font_) DeleteObject(font_);
        if (brush_) DeleteObject(brush_);
    }

    [[nodiscard]] HWND window() const noexcept { return window_; }

    bool show(HWND owner, const std::wstring& report) {
        report_ = report;
        if (!window_) {
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof(cls); cls.hInstance = instance;
            cls.lpfnWndProc = &procedure; cls.lpszClassName = L"RifeLoveNativeStatusWindow";
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            const UINT dpi = GetDpiForWindow(owner);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
            const RECT work = monitor.rcWork;
            const int width = std::min<int>(MulDiv(820, dpi ? dpi : 96, 96), work.right - work.left);
            const int height = std::min<int>(MulDiv(560, dpi ? dpi : 96, 96), work.bottom - work.top);
            window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"RifeLove · 当前任务详情",
                WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2,
                work.top + (work.bottom - work.top - height) / 2, width, height,
                owner, nullptr, instance, this);
            if (!window_) return false;
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        }
        SetWindowTextW(text_, report_.c_str());
        SendMessageW(text_, EM_SETSEL, 0, 0);
        SendMessageW(text_, EM_SCROLLCARET, 0, 0);
        SetWindowTextW(copy_, L"复制全部");
        ShowWindow(window_, SW_SHOWNORMAL);
        SetForegroundWindow(window_);
        SetFocus(text_);
        return true;
    }

    bool handleDialogMessage(MSG& message) {
        if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(window_); return true;
        }
        if (message.hwnd == text_ && message.message == WM_KEYDOWN && message.wParam == 'A' &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(text_, EM_SETSEL, 0, -1); return true;
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }

private:
    int px(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }

    void setFont() {
        const HFONT next = CreateFontW(-MulDiv(11, GetDpiForWindow(window_), 72), 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (!next) return;
        for (const auto control : {hint_, text_, copy_, close_})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font_) DeleteObject(font_);
        font_ = next;
    }

    void layout() {
        RECT client{}; GetClientRect(window_, &client);
        const int width = client.right, height = client.bottom;
        MoveWindow(hint_, px(18), px(12), std::max(1, width - px(36)), px(24), TRUE);
        MoveWindow(text_, px(18), px(44), std::max(1, width - px(36)), std::max(1, height - px(108)), TRUE);
        MoveWindow(copy_, std::max(px(18), width - px(254)), height - px(48), px(136), px(32), TRUE);
        MoveWindow(close_, std::max(px(162), width - px(108)), height - px(48), px(90), px(32), TRUE);
    }

    bool copyReport() const {
        const SIZE_T bytes = (report_.size() + 1) * sizeof(wchar_t);
        const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) return false;
        void* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); return false; }
        std::memcpy(target, report_.c_str(), bytes); GlobalUnlock(memory);
        if (!OpenClipboard(window_)) { GlobalFree(memory); return false; }
        const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        CloseClipboard();
        if (!copied) GlobalFree(memory);
        return copied;
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<NativeStatusWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<NativeStatusWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->window_ = window;
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CREATE: {
            self->brush_ = CreateSolidBrush(RGB(19, 24, 32));
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            self->hint_ = CreateWindowExW(0, L"STATIC", L"当前状态快照 · 可选择文本、Ctrl+A / Ctrl+C；重新打开可刷新。",
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
            self->text_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(1401), instance, nullptr);
            self->copy_ = CreateWindowExW(0, L"BUTTON", L"复制全部",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(1402), instance, nullptr);
            self->close_ = CreateWindowExW(0, L"BUTTON", L"关闭",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);
            if (!self->hint_ || !self->text_ || !self->copy_ || !self->close_) return -1;
            SendMessageW(self->text_, EM_SETLIMITTEXT, 8U * 1024U * 1024U, 0);
            self->setFont(); return 0;
        }
        case WM_SIZE: self->layout(); return 0;
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->setFont(); self->layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = POINT{self->px(360), self->px(220)}; return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(window); return 0; }
            if (LOWORD(wParam) == 1402 && HIWORD(wParam) == BN_CLICKED) {
                SetWindowTextW(self->copy_, self->copyReport() ? L"已复制" : L"复制失败，请重试"); return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item || item->CtlType != ODT_BUTTON) break;
            const int saved = SaveDC(item->hDC);
            const HBRUSH fill = CreateSolidBrush(item->itemState & ODS_SELECTED ? RGB(42, 72, 91) : RGB(34, 56, 82));
            const auto priorBrush = SelectObject(item->hDC, fill);
            const auto priorPen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, self->px(8), self->px(8));
            SelectObject(item->hDC, priorPen); SelectObject(item->hDC, priorBrush); DeleteObject(fill);
            SetTextColor(item->hDC, RGB(238, 242, 249)); SetBkMode(item->hDC, TRANSPARENT);
            SelectObject(item->hDC, self->font_);
            wchar_t label[48]{}; GetWindowTextW(item->hwndItem, label, 48);
            RECT rect = item->rcItem; DrawTextW(item->hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (item->itemState & ODS_FOCUS) { InflateRect(&rect, -self->px(4), -self->px(4)); DrawFocusRect(item->hDC, &rect); }
            if (saved) RestoreDC(item->hDC, saved);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            RECT client{}; GetClientRect(window, &client); FillRect(reinterpret_cast<HDC>(wParam), &client, self->brush_); return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(238, 242, 249));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(19, 24, 32));
            return reinterpret_cast<LRESULT>(self->brush_);
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_NCDESTROY:
            self->window_ = nullptr; self->hint_ = nullptr; self->text_ = nullptr; self->copy_ = nullptr; self->close_ = nullptr;
            if (self->brush_) { DeleteObject(self->brush_); self->brush_ = nullptr; }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        default: break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_ = nullptr, hint_ = nullptr, text_ = nullptr, copy_ = nullptr, close_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH brush_ = nullptr;
    std::wstring report_;
};

} // namespace rifelove::native_desktop

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace rifelove::native_desktop {

[[nodiscard]] inline std::wstring compactClosedSelectorLabel(std::wstring_view label) {
    if (label.starts_with(L"关闭（默认：")) return L"关闭";
    if (label == L"自动（NVENC 优先）") return L"自动 · NVENC";
    if (label == L"本机高帧率母版 / 自定义") return L"本机母版 · 自定义";
    if (label == L"默认：原尺寸 / 超分 ×2") return L"默认尺寸";
    if (label == L"未选择压缩修复模型") return L"未选择模型";
    if (label.ends_with(L" 本地副本")) label.remove_suffix(std::wstring_view(L" 本地副本").size());
    return std::wstring(label);
}

struct NativeRenderState {
    std::wstring sourcePath;
    std::wstring outputPath;
    std::wstring mediaSummary;
    std::wstring modelSummary;
    std::wstring runtimeSummary;
    std::wstring status;
    std::wstring deviceSummary;
    std::wstring encoderSummary;
    std::wstring platformSummary;
    std::wstring motionBlurSummary;
    std::wstring restorationSummary;
    std::wstring denoiseModeSummary;
    std::wstring denoiseAdjustmentSummary;
    std::wstring detailSummary;
    std::wstring detailAdjustmentSummary;
    std::wstring compressionSummary;
    std::wstring compressionAdjustmentSummary;
    std::wstring outputSizeSummary;
    std::wstring stage;
    std::size_t outputFrames = 0;
    double processingFps = 0.0;
    bool processing = false;
    bool processingAvailable = false;
    bool sourceReady = false;
    bool deviceReady = false;
    bool encoderReady = false;
    bool modelReady = false;
    bool runtimeReady = false;
    bool advancedExpanded = false;
    bool qualityExpanded = false;
    bool homePage = false;
    bool automaticTemporalDenoise = false;
    bool automaticDetailRestoration = false;
    bool automaticCompressionRestoration = false;
    float scrollOffset = 0.0F;
};

// One layout contract is shared by HWND controls and the Direct2D surface.
// Measurements are device independent; the action dock remains visible while
// the settings viewport scrolls on small screens or at increased display DPI.
struct NativeLayout {
    static constexpr float denoiseAdjustmentHeight = 94.0F;
    static constexpr float deblurAdjustmentHeight = 204.0F;
    static constexpr float compressionAdjustmentHeight = 140.0F;
    // Room for the disclosure row and its separation from optional controls.
    static constexpr float qualityDisclosureHeight = 130.0F;
    static constexpr float collapsedQualityHeight = 166.0F;
    static constexpr float qualityControlsHeight = denoiseAdjustmentHeight + deblurAdjustmentHeight + compressionAdjustmentHeight;
    float width = 1366;
    float height = 768;
    float side = 188;
    float left = 214;
    float right = 1340;
    float split = 884;
    float dock = 660;
    float contentTop = 92;
    float contentBottom = 626;
    float resourceButtonsTop = 410;
    bool singleColumn = false;
    bool qualityExpanded = false;
    bool compactHome = false;
    float exportOffset = 0;
    float contentEnd = 778 + qualityControlsHeight;
    bool compactImport = false;
    float sourceActionsTop = 116;
    float sourceInputTop = 182;
    float scroll = 0;
    float homeHeroHeight = 224;
    float homeStartTop = 266;
    float homeStartHeight = 46;
    float homeReadinessTop = 350;
    float homeReadinessHeight = 176;
    float homeReadinessGap = 14;
    float homeReadinessWidth = 0;
    // Export card and HWND controls use the same 20-DIP content inset.
    [[nodiscard]] float exportCardLeft() const { return singleColumn ? left : split + 20.0F; }
    [[nodiscard]] float exportCardWidth() const { return right - exportCardLeft(); }
    [[nodiscard]] float exportContentLeft() const { return exportCardLeft() + 20.0F; }
    [[nodiscard]] float exportContentWidth() const { return std::max(0.0F, exportCardWidth() - 40.0F); }
    [[nodiscard]] float outputBrowseWidth() const { return std::min(120.0F, exportContentWidth()); }
    [[nodiscard]] float outputBrowseLeft() const { return right - 20.0F - outputBrowseWidth(); }
    [[nodiscard]] static NativeLayout from(float width, float height, float scroll = 0,
                                           bool qualityExpanded = false) {
        NativeLayout value;
        value.width = width; value.height = height;
        value.qualityExpanded = qualityExpanded;
        value.singleColumn = width < 760.0F;
        value.side = value.singleColumn ? 64.0F : width < 1100 ? 164.0F : 188.0F;
        value.left = value.side + (value.singleColumn ? 16.0F : 24.0F);
        value.right = width - (value.singleColumn ? 16.0F : 24.0F);
        // Narrow windows need more room for the output path and encoder labels.
        // Keep the source column wide enough for its compact import controls,
        // then smoothly restore the standard source-first balance as the
        // available window width grows.
        const float wideWindowMix = std::clamp((width - 760.0F) / 340.0F, 0.0F, 1.0F);
        const float sourceShare = 0.48F + 0.07F * wideWindowMix;
        value.split = value.singleColumn ? value.right :
            value.left + (value.right - value.left - 20.0F) * sourceShare;
        value.exportOffset = value.singleColumn ?
            (qualityExpanded ? 690.0F + qualityControlsHeight + qualityDisclosureHeight : 406.0F) : 0.0F;
        value.contentEnd = std::max(qualityExpanded ?
            778.0F + qualityControlsHeight + qualityDisclosureHeight : 296.0F + collapsedQualityHeight,
            626.0F + value.exportOffset);
        // Two import actions share their own row when the source card is too
        // narrow to hold both its title and the file/folder buttons.
        value.compactImport = value.split - value.left < 440.0F;
        value.sourceActionsTop = value.compactImport ? 150.0F : 116.0F;
        value.sourceInputTop = value.compactImport ? 194.0F : 182.0F;
        // Home has a different, shorter responsive layout from the scrollable
        // Studio. Stack readiness summaries before their text becomes cramped.
        const float homeWidth = value.right - value.left;
        value.compactHome = homeWidth < 620.0F;
        if (value.compactHome) {
            value.homeHeroHeight = 164.0F;
            value.homeStartTop = 216.0F;
            value.homeStartHeight = 42.0F;
            value.homeReadinessTop = 280.0F;
            value.homeReadinessGap = 8.0F;
            value.homeReadinessHeight = std::clamp(
                (height - value.homeReadinessTop - 16.0F - value.homeReadinessGap * 2.0F) / 3.0F,
                44.0F, 80.0F);
            value.homeReadinessWidth = homeWidth;
        } else {
            value.homeReadinessTop = height < 520.0F ? 338.0F : 350.0F;
            value.homeReadinessHeight = std::max(120.0F,
                std::min(176.0F, height - value.homeReadinessTop - 20.0F));
            value.homeReadinessWidth = (homeWidth - value.homeReadinessGap * 2.0F) / 3.0F;
        }
        value.dock = height - (value.singleColumn ? 138.0F : 100.0F);
        value.contentBottom = value.dock - 14.0F;
        // Keep both resource buttons inside short windows at high DPI.
        value.resourceButtonsTop = std::min(410.0F, std::max(18.0F, height - 90.0F));
        value.scroll = std::clamp(scroll, 0.0F, std::max(0.0F, value.contentEnd - value.contentBottom));
        return value;
    }
};

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] T* get() const noexcept { return value_; }
    [[nodiscard]] T** put() noexcept { reset(); return &value_; }
    [[nodiscard]] T* operator->() const noexcept { return value_; }
    void reset() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }
private:
    T* value_ = nullptr;
};

class NativeRenderer final {
public:
    NativeRenderer() = default;
    ~NativeRenderer() = default;
    NativeRenderer(const NativeRenderer&) = delete;
    NativeRenderer& operator=(const NativeRenderer&) = delete;

    [[nodiscard]] bool render(HWND window, const NativeRenderState& state);
    [[nodiscard]] bool renderToDc(HWND window, HDC dc, const NativeRenderState& state);
    void discardTarget() noexcept;

private:
    [[nodiscard]] bool ensureResources(HWND window);
    [[nodiscard]] bool draw(HWND window, const NativeRenderState& state);
    void drawText(const std::wstring& text, float left, float top, float right, float bottom,
        float size, D2D1::ColorF color, bool bold = false, bool wrap = false);
    void fillCard(float left, float top, float right, float bottom, D2D1::ColorF color, float radius = 12.0F);
    void drawPill(const std::wstring& text, float left, float top, float right, float bottom,
        D2D1::ColorF fill, D2D1::ColorF textColor);

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<ID2D1RenderTarget> renderTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> writeFactory_;
    HWND targetWindow_ = nullptr;
    UINT targetWidth_ = 0;
    UINT targetHeight_ = 0;
    float scale_ = 1.0F;
};

} // namespace rifelove::native_desktop

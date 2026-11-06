#include "native_renderer.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace rifelove::native_desktop {
namespace {
const D2D1::ColorF background(0x0C0F15);
const D2D1::ColorF sidebar(0x10141C);
const D2D1::ColorF card(0x131820);
const D2D1::ColorF cardBorder(0x252D3B);
const D2D1::ColorF white(0xEEF2F9);
const D2D1::ColorF muted(0x8895A9);
const D2D1::ColorF accent(0xA2E7CB);
const D2D1::ColorF blue(0xB2BEFF);
const D2D1::ColorF warning(0xF4C77B);
const D2D1::ColorF error(0xFF9B9B);

float dip(float value, float scale) { return value * scale; }
}

bool NativeRenderer::ensureResources(HWND window) {
    if (!d2dFactory_.get()) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.put()))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory_.put())))) return false;
    }
    const UINT dpi = GetDpiForWindow(window);
    scale_ = static_cast<float>(dpi ? dpi : 96U) / 96.0F;
    RECT client{};
    GetClientRect(window, &client);
    const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
    if (!renderTarget_.get() || targetWindow_ != window || width != targetWidth_ || height != targetHeight_) {
        renderTarget_.reset();
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        const auto hwndProperties = D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(width, height));
        ComPtr<ID2D1HwndRenderTarget> target;
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(properties, hwndProperties, target.put()))) return false;
        if (FAILED(target->QueryInterface(__uuidof(ID2D1RenderTarget), reinterpret_cast<void**>(renderTarget_.put())))) return false;
        renderTarget_->SetDpi(96, 96);
        targetWindow_ = window;
        targetWidth_ = width;
        targetHeight_ = height;
        if (FAILED(renderTarget_->CreateSolidColorBrush(white, brush_.put()))) return false;
    }
    return true;
}

void NativeRenderer::discardTarget() noexcept {
    renderTarget_.reset();
    brush_.reset();
    targetWindow_ = nullptr;
    targetWidth_ = 0;
    targetHeight_ = 0;
}

void NativeRenderer::fillCard(float left, float top, float right, float bottom, D2D1::ColorF color, float radius) {
    brush_->SetColor(color);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), radius, radius), brush_.get());
    brush_->SetColor(cardBorder);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), radius, radius), brush_.get(), 1.0F);
}

void NativeRenderer::drawPill(const std::wstring& text, float left, float top, float right, float bottom,
    D2D1::ColorF fill, D2D1::ColorF textColor) {
    brush_->SetColor(fill);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), 18.0F, 18.0F), brush_.get());
    drawText(text, left + dip(13.0F, scale_), top + dip(3.0F, scale_), right - dip(10.0F, scale_), bottom,
        12.0F, textColor, false, false);
}

void NativeRenderer::drawText(const std::wstring& text, float left, float top, float right, float bottom,
    float size, D2D1::ColorF color, bool bold, bool wrap) {
    if (text.empty()) return;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, dip(size, scale_),
        L"zh-CN", format.put()))) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (!wrap && SUCCEEDED(writeFactory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.get());
    }
    brush_->SetColor(color);
    renderTarget_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format.get(),
        D2D1::RectF(left, top, right, bottom), brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool NativeRenderer::render(HWND window, const NativeRenderState& state) {
    if (!ensureResources(window)) return false;
    return draw(window, state);
}

bool NativeRenderer::renderToDc(HWND window, HDC dc, const NativeRenderState& state) {
    if (!dc || !ensureResources(window)) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    ComPtr<ID2D1DCRenderTarget> target;
    if (FAILED(d2dFactory_->CreateDCRenderTarget(&properties, target.put())) ||
        FAILED(target->BindDC(dc, &client))) return false;

    auto previousTarget = std::move(renderTarget_);
    auto previousBrush = std::move(brush_);
    bool rendered = false;
    if (SUCCEEDED(target->QueryInterface(__uuidof(ID2D1RenderTarget), reinterpret_cast<void**>(renderTarget_.put()))) &&
        SUCCEEDED(renderTarget_->CreateSolidColorBrush(white, brush_.put()))) {
        rendered = draw(window, state);
    }
    brush_.reset();
    renderTarget_.reset();
    renderTarget_ = std::move(previousTarget);
    brush_ = std::move(previousBrush);
    return rendered;
}

bool NativeRenderer::draw(HWND window, const NativeRenderState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const auto l = NativeLayout::from(static_cast<float>(client.right) / scale_,
        static_cast<float>(client.bottom) / scale_, state.scrollOffset, state.qualityExpanded);
    const auto px = [&](float n) { return dip(n, scale_); };
    const auto text = [&](const std::wstring& value, float x, float y, float w, float h,
        float size = 11, D2D1::ColorF color = muted, bool bold = false, bool wrap = false) {
        // The original 10–11 DIP labels read too small at the app's normal
        // 1366×768 window size. Scale the whole type system slightly while
        // retaining the existing card and control geometry.
        drawText(value, px(x), px(y), px(x + w), px(y + h), size * 1.1F, color, bold, wrap);
    };
    const auto cardAt = [&](float x, float y, float w, float h, D2D1::ColorF fill = card, float r = 12) {
        fillCard(px(x), px(y), px(x + w), px(y + h), fill, px(r));
    };
    const auto line = [&](float x, float y, float w, D2D1::ColorF color = cardBorder) {
        brush_->SetColor(color);
        renderTarget_->DrawLine(D2D1::Point2F(px(x), px(y)), D2D1::Point2F(px(x + w), px(y)), brush_.get(), px(1));
    };
    renderTarget_->BeginDraw();
    renderTarget_->Clear(background);
    brush_->SetColor(sidebar);
    renderTarget_->FillRectangle(D2D1::RectF(0, 0, px(l.side), px(l.height)), brush_.get());
    line(0, 84, l.side);
    brush_->SetColor(cardBorder);
    renderTarget_->DrawLine(D2D1::Point2F(px(l.side), 0), D2D1::Point2F(px(l.side), px(l.height)), brush_.get());
    cardAt(18, 24, 32, 32, D2D1::ColorF(0xA2E7CB), 9);
    text(L"R", 27, 25, 24, 30, 18, D2D1::ColorF(0x10241D), true);
    if (!l.singleColumn) {
    text(L"RifeLove", 61, 23, l.side - 67, 25, 17, white, true);
    text(L"视频增强工作台", 61, 50, l.side - 67, 18, 10, muted);
    text(L"工作空间", 22, 107, l.side - 44, 18, 10, muted);
    if (!l.singleColumn && l.resourceButtonsTop >= 280)
        text(state.processingAvailable ? L"本地历史与断点恢复" : L"任务记录 · 暂不可用",
             28, state.processingAvailable ? 271 : 271, l.side - 45, 20, 10, D2D1::ColorF(0x9AAAC0));
    if (!l.singleColumn && l.resourceButtonsTop >= 340) {
        text(state.processingAvailable ? L"模型中心 · 本地与 HTTPS" : L"模型中心 · 当前构建不可用",
             28, 298, l.side - 50, 20, 10, D2D1::ColorF(0x9AAAC0));
        text(L"完整性校验 · 显式安装", 28, 318, l.side - 45, 18, 10, D2D1::ColorF(0x667286));
    }
    if (!l.singleColumn && l.resourceButtonsTop >= 380) {
        line(20, 350, l.side - 40);
        text(L"本地资源", 22, 365, l.side - 44, 20, 10, muted);
    }
    if (!l.singleColumn && l.resourceButtonsTop >= 400)
        text(state.modelReady ? L"●  RIFE 模型已校验" : L"○  RIFE 模型未就绪", 22, 389, l.side - 40, 21, 10, state.modelReady ? accent : warning);
    // Runtime discovery is shown near the actual folder selectors.
    if (l.height > 620 && !l.singleColumn) {
        line(20, l.height - 136, l.side - 40);
        text(L"NVIDIA GPU", 22, l.height - 115, l.side - 44, 19, 10, accent, true);
        text(state.deviceSummary.empty() ? L"导入后检查本机设备" : state.deviceSummary,
            22, l.height - 89, l.side - 44, 52, 11, white, false, true);
        text(L"离线处理 · 源文件保持不变", 22, l.height - 29, l.side - 44, 19, 10, muted);
    }
    } else {
        text(L"菜单", 18, 111, 40, 20, 10, muted);
        if (l.height > 580) text(L"本地", 18, l.height - 38, 40, 20, 10, accent);
    }
    if (state.homePage) {
        text(L"RIFELOVE  ·  本地视频工作台", l.left, 18, l.right - l.left - 260, 20, 10, accent, true);
        text(L"欢迎使用 RifeLove", l.left, 42, l.right - l.left - 260, 38, 25, white, true);
        drawPill(state.processing ? L"●  正在处理" : L"●  完全本地", px(l.right - 126), px(30), px(l.right), px(60), D2D1::ColorF(0x1B2C27), accent);

        const float bodyWidth = l.right - l.left;
        cardAt(l.left, 104, bodyWidth, 224, D2D1::ColorF(0x141B25), 16);
        text(L"AI 帧插值", l.left + 28, 125, bodyWidth - 56, 22, 11, accent, true);
        text(L"让每一段运动，更流畅。", l.left + 28, 151, bodyWidth - 56, 38, l.singleColumn ? 21 : 27, white, true);
        text(state.processing
                ? std::to_wstring(state.outputFrames) + L" 帧已输出 · " +
                    std::to_wstring(static_cast<int>(state.processingFps * 10.0) / 10.0) + L" 处理 FPS"
                : L"选择视频后，RifeLove 会先检查素材、模型与运行环境，再生成新的中间帧。",
             l.left + 30, 202, bodyWidth - 60, 44, 12, muted, false, true);
        text(state.processing && !state.status.empty() ? state.status :
             L"原始视频保持不变 · 推理与导出都在本机完成", l.left + 30, 242, bodyWidth - 60, 22, 10, muted);

        const float gap = 14.0F;
        const float cardWidth = (bodyWidth - gap * 2.0F) / 3.0F;
        const float cardsTop = l.height < 520.0F ? 338.0F : 350.0F;
        const float cardHeight = std::max(120.0F, std::min(176.0F, l.height - cardsTop - 20.0F));
        const auto readinessCard = [&](float x, const wchar_t* eyebrow, const wchar_t* heading,
            const std::wstring& detail, bool ready) {
            cardAt(x, cardsTop, cardWidth, cardHeight);
            text(eyebrow, x + 16, cardsTop + 16, cardWidth - 32, 18, 9, muted, true);
            text(heading, x + 16, cardsTop + 40, cardWidth - 32, 24, 14, white, true);
            text(ready ? L"已就绪" : L"需要设置", x + 16, cardsTop + 69, cardWidth - 32, 20, 10, ready ? accent : warning, true);
            text(detail, x + 16, cardsTop + 96, cardWidth - 32, cardHeight - 106, 10, muted, false, true);
        };
        std::wstring deviceDetail = L"导入视频后检查 NVIDIA GPU、CUDA 驱动与可用编码器。";
        if (state.deviceReady || state.encoderReady) {
            deviceDetail.clear();
            if (!state.deviceSummary.empty()) deviceDetail = state.deviceSummary;
            if (!state.encoderSummary.empty()) {
                if (!deviceDetail.empty()) deviceDetail += L"\n";
                deviceDetail += state.encoderSummary;
            }
        }
        readinessCard(l.left, L"01  ·  硬件", L"设备与编码",
            deviceDetail, state.deviceReady && state.encoderReady);
        readinessCard(l.left + cardWidth + gap, L"02  ·  MODEL", L"RIFE 模型",
            state.modelReady ? (state.modelSummary.empty() ? L"默认模型已通过本机完整性校验。" : state.modelSummary)
                : L"在本地模型中心安装并验证 RIFE 模型。",
            state.modelReady);
        readinessCard(l.left + (cardWidth + gap) * 2.0F, L"03  ·  RUNTIME", L"推理运行库",
            state.runtimeReady ? (state.runtimeSummary.empty() ? L"本地推理运行库已就绪。" : state.runtimeSummary)
                : L"选择本地 CUDA / TensorRT 运行库。",
            state.runtimeReady);
        if (l.height >= 640.0F) {
            cardAt(l.left, 548, bodyWidth, 120, D2D1::ColorF(0x111720), 14);
            text(L"从视频到成片", l.left + 20, 566, 170, 22, 12, white, true);
            const float stepLeft = l.left + 205;
            const float stepWidth = std::max(90.0F, (bodyWidth - 225.0F) / 3.0F);
            const std::array<std::wstring, 3> steps{L"01  导入视频", L"02  选择帧率与编码", L"03  预览并导出"};
            for (std::size_t i = 0; i < steps.size(); ++i) {
                const float x = stepLeft + (stepWidth + 8.0F) * static_cast<float>(i);
                text(steps[i], x, 570, stepWidth, 26, 11, i == 0 ? accent : muted, i == 0, true);
            }
            text(L"导出前会再次检查目标路径、帧数与媒体时间轴。", l.left + 205, 603, bodyWidth - 225, 36, 10, muted, false, true);
        }
        const HRESULT result = renderTarget_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) discardTarget();
        return SUCCEEDED(result);
    }

    text(L"工作室", l.left, 17, 200, 19, 10, muted);
    text(l.singleColumn ? L"插帧" : L"让每一段运动，更流畅。", l.left, 39, l.right - l.left - 280, 36, l.singleColumn ? 18 : 23, white, true);
    if (l.contentBottom < l.contentEnd)
        text(l.scroll < l.contentEnd - l.contentBottom ? L"更多设置 ↓  ·  滚轮滚动 / Tab 自动定位" : L"返回上方 ↑  ·  滚轮 / Shift+Tab 自动定位",
            l.left, 76, l.right - l.left, 16, 10, blue);
    drawPill(state.processing ? L"●  正在处理" : L"●  完全本地", px(l.right - 126), px(30), px(l.right), px(60), D2D1::ColorF(0x1B2C27), accent);

    renderTarget_->PushAxisAlignedClip(D2D1::RectF(px(l.left - 1), px(l.contentTop), px(l.right + 1), px(l.contentBottom)), D2D1_ANTIALIAS_MODE_ALIASED);
    const float y = -l.scroll;
    const float lw = l.split - l.left;
    const float second = l.exportCardLeft();
    const float rw = l.exportCardWidth();
    const float exportLeft = l.exportContentLeft();
    const float exportWidth = l.exportContentWidth();
    const float ey = y + l.exportOffset;
    cardAt(l.left, y + 104, lw, 176);
    text(L"01", l.left + 20, y + 118, 28, 22, 10, accent, true);
    text(L"源视频", l.left + 53, y + 116, lw - (l.compactImport ? 84 : 190), 26, 14, white, true);
    if (!l.compactImport)
        text(state.sourceReady ? L"视频已解析 · 原片不会被修改" : L"拖入视频，或从本机选择文件",
            l.left + 20, y + 157, lw > 480 ? lw - 240 : lw - 40, 23, 12, state.sourceReady ? white : muted);
    if (lw > 480)
        text(L"悬停或右键查看完整路径", l.left + lw - 220, y + 159, 200, 20, 10, accent, false, false);
    cardAt(l.left + 20, y + l.sourceInputTop, lw - 40, 42, D2D1::ColorF(0x141922), 7);
    text(state.sourcePath.empty() ? L"尚未选择视频" : state.sourcePath,
        l.left + 20, y + l.sourceInputTop + 46, lw - 40, 18, 10, white);
    text(state.mediaSummary.empty() ? L"MP4 / MKV / MOV  ·  精确时间轴" : state.mediaSummary,
        l.left + 20, y + l.sourceInputTop + 66, lw - 40, 18, 10, muted);

    cardAt(l.left, y + 296, lw, state.qualityExpanded ?
        482 + NativeLayout::qualityControlsHeight + NativeLayout::qualityDisclosureHeight :
        NativeLayout::collapsedQualityHeight);
    text(L"02", l.left + 20, y + 311, 28, 22, 10, blue, true);
    text(L"画质与运动", l.left + 53, y + 309, lw - 84, 26, 14, white, true);
    if (!state.qualityExpanded) {
        text(L"可选增强：降噪 · 去模糊 · 超分 · 压缩修复",
            l.left + 20, y + 421, lw - 40, 22, 10, muted);
    } else {
    // The model controls begin immediately after the disclosure row.
    D2D1::Matrix3x2F qualityTransform;
    renderTarget_->GetTransform(&qualityTransform);
    renderTarget_->SetTransform(qualityTransform *
        D2D1::Matrix3x2F::Translation(0, px(NativeLayout::qualityDisclosureHeight)));
    // The native controls are separate so denoise, deblur and super-resolution
    // can be enabled independently. Keep their labels close
    // to the corresponding fields so long model names remain understandable.
    text(state.denoiseModeSummary.empty() ? L"可选降噪 · 默认项见模型中心" : state.denoiseModeSummary,
        l.left + 20, y + 331, lw - 40, 20, 11, state.automaticTemporalDenoise ? warning : muted);
    text(L"降噪相对自动值（%）", l.left + 20, y + 397, lw - 40, 20, 11, muted);
    cardAt(l.left + 20, y + 420, lw - 130, 30, D2D1::ColorF(0x141922), 7);
    text(state.denoiseAdjustmentSummary.empty() ? L"自动模式关闭：选择降噪模型后可启用。" : state.denoiseAdjustmentSummary,
        l.left + 20, y + 456, lw - 40, 32, 10, state.automaticTemporalDenoise ? warning : muted, false, true);
    const float deblurTop = 397 + NativeLayout::denoiseAdjustmentHeight;
    text(L"去模糊 · 实验性，默认关闭", l.left + 20, y + deblurTop, lw - 40, 20, 11, muted);
    text(L"动态细节", l.left + 20, y + deblurTop + 66, lw - 160, 20, 11, muted);
    text(L"细节相对自动值（%）", l.left + 20, y + deblurTop + 104, lw - 40, 20, 11, muted);
    cardAt(l.left + 20, y + deblurTop + 127, lw - 130, 30, D2D1::ColorF(0x141922), 7);
    text(state.detailAdjustmentSummary.empty() ? L"自动模式关闭；选中模型仍使用固定去模糊。" : state.detailAdjustmentSummary,
        l.left + 20, y + deblurTop + 163, lw - 40, 34, 10,
        state.automaticDetailRestoration ? warning : muted, false, true);
    const float outputImageOffset = NativeLayout::denoiseAdjustmentHeight + NativeLayout::deblurAdjustmentHeight;
    text(L"超分辨率 · 默认项见模型中心", l.left + 20, y + 397 + outputImageOffset, lw - 40, 20, 11, muted);
    text(L"输出尺寸 · 仅超分模型可用", l.left + 20, y + 463 + outputImageOffset, lw - 40, 20, 11, muted);
    text(L"压缩修复 · STDF 本机评估", l.left + 20, y + 529 + outputImageOffset, (lw - 48) * 0.68F, 20, 11, muted);
    text(state.automaticCompressionRestoration ? L"手动值（停用）" : L"强度 / 开关", l.left + 28 + (lw - 48) * 0.68F,
        y + 529 + outputImageOffset, (lw - 48) * 0.32F, 20, 11, muted);
    text(L"动态压缩修复", l.left + 20, y + 635 + outputImageOffset, lw - 170, 20, 11, muted);
    text(L"压缩修复相对自动值（%）", l.left + 20, y + 670 + outputImageOffset, lw - 40, 20, 11, muted);
    cardAt(l.left + 20, y + 693 + outputImageOffset, lw - 130, 30, D2D1::ColorF(0x141922), 7);
    text(state.compressionAdjustmentSummary.empty() ? L"自动模式关闭；手动强度保持独立。" : state.compressionAdjustmentSummary,
        l.left + 20, y + 729 + outputImageOffset, lw - 40, 36, 10,
        state.automaticCompressionRestoration ? warning : muted, false, true);
    text(state.compressionSummary.empty() ? L"默认关闭 · CUDA / SDR8 · 仅验证至 384 × 256" : state.compressionSummary,
        l.left + 20, y + 634 + NativeLayout::qualityControlsHeight, lw - 40, 34, 10, warning, false, true);
    line(l.left + 20, y + 672 + NativeLayout::qualityControlsHeight, lw - 40);
    if (state.advancedExpanded) {
        text(L"快门角度", l.left + 20, y + 737 + NativeLayout::qualityControlsHeight, 64, 20, 10, muted);
        text(L"采样数", l.left + (lw - 40 < 280 ? 154 : 170), y + 737 + NativeLayout::qualityControlsHeight, lw - 40 < 280 ? 54 : 62, 20, 10, muted);
    } else {
        text(state.motionBlurSummary, l.left + 20, y + 729 + NativeLayout::qualityControlsHeight, lw - 40, 38, 11, muted, false, true);
    }
    line(l.left + 20, y + 776 + NativeLayout::qualityControlsHeight, lw - 40);
    renderTarget_->SetTransform(qualityTransform);
    }

    cardAt(second, ey + 104, rw, 522);
    text(L"03", exportLeft, ey + 118, 28, 22, 10, accent, true);
    text(L"导出设置", second + 53, ey + 116, rw - 78, 26, 14, white, true);
    text(L"导出预设", exportLeft, ey + 141, exportWidth, 18, 10, muted);
    text(L"目标帧率", exportLeft, ey + 217, exportWidth, 20, 11, muted);
    text(L"编码器", exportLeft, ey + 295, exportWidth, 20, 11, muted);
    text(L"输出文件", exportLeft, ey + 374, exportWidth, 20, 11, muted);
    if (rw > 360)
        text(L"悬停或右键查看完整路径", second + rw - 220, ey + 375, 200, 20, 10, accent, false, false);
    cardAt(exportLeft, ey + 394, exportWidth, 42, D2D1::ColorF(0x141922), 7);
    text(state.outputPath.empty() ? L"尚未设置输出文件" : state.outputPath,
        exportLeft, ey + 440, exportWidth, 18, 10, white);
    line(exportLeft, ey + 498, exportWidth);
    text(state.platformSummary, exportLeft, ey + 511, exportWidth, 78, 11, muted, false, true);
    text(L"仅本地导出 · 不上传 · 不承诺平台播放帧率", exportLeft, ey + 594, exportWidth, 23, 10, muted);
    renderTarget_->PopAxisAlignedClip();

    // The primary action dock is never scrolled off-screen.
    line(l.left, l.dock, l.right - l.left);
    const auto statusColor = state.stage == L"failed" ? error : state.processing ? blue : state.stage == L"completed" ? accent : muted;
    const bool paused = state.stage == L"已暂停";
    text(paused ? L"已暂停" : state.processing ? L"正在处理" : state.stage == L"completed" ? L"处理完成" : L"任务状态",
        l.left + 2, l.dock + (l.singleColumn ? 8 : 19), 70, 22, 11, statusColor, true);
    std::wstring status = state.status.empty() ? L"选择源视频开始检查" : state.status;
    if (state.processing && !paused)
        status = std::to_wstring(state.outputFrames) + L" 帧  ·  " + std::to_wstring(static_cast<int>(state.processingFps * 10.0) / 10.0) + L" 处理 FPS  ·  " + status;
    else if (paused) status = L"已生成 " + std::to_wstring(state.outputFrames) + L" 帧  ·  " + status;
    text(status, l.left + 2, l.dock + (l.singleColumn ? 32 : 43), l.right - l.left - (l.singleColumn ? 4 : 360), 42, 11, statusColor, false, true);
    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardTarget();
    return SUCCEEDED(result);
}

} // namespace rifelove::native_desktop

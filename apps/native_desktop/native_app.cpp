#include "native_app.hpp"
#include "application/platform_bitrate.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <commctrl.h>
#include <commdlg.h>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <cstdlib>
#include <dwmapi.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <shlobj.h>
#include <shellapi.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "native_package_archive.hpp"
#include "native_model_download.hpp"
#include <nlohmann/json.hpp>
#include "media/encoder_policy.hpp"
#include "media/media_engine.hpp"
#include "media/rife_color.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/video_restoration_backend.hpp"
#include "inference/model_pack.hpp"
#endif

namespace rifelove::native_desktop {
namespace {
constexpr wchar_t kWindowClass[] = L"RifeLoveNativeDesktopWindow";
constexpr wchar_t kWindowTitle[] = L"RifeLove";
constexpr wchar_t kPathEditOwnerProperty[] = L"RifeLovePathEditOwner";
constexpr wchar_t kPathEditOriginalProcProperty[] = L"RifeLovePathEditOriginalProc";
constexpr wchar_t kPathTooltipProperty[] = L"RifeLovePathTooltip";
constexpr wchar_t kPathTooltipTextProperty[] = L"RifeLovePathTooltipText";
constexpr std::array<int, 9> kTargetRates{30, 60, 120, 144, 240, 300, 360, 480, 600};
constexpr UINT kPathMenuView = 1;
constexpr UINT kPathMenuCopy = 2;
constexpr UINT kPathMenuCopySelection = 3;
constexpr UINT kPathMenuCut = 4;
constexpr UINT kPathMenuPaste = 5;
constexpr UINT kPathMenuSelectAll = 6;

int scaled(int value, float scale) { return static_cast<int>(static_cast<float>(value) * scale); }

bool ensureTooltipControls() {
    static const bool ready = [] {
        // Keep the system common-controls module loaded for the tooltip HWNDs'
        // lifetime without adding a framework or changing the executable link.
        const HMODULE module = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        using Initialize = BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*);
        const auto entryPoint = GetProcAddress(module, "InitCommonControlsEx");
        static_assert(sizeof(Initialize) == sizeof(entryPoint));
        const auto initialize = std::bit_cast<Initialize>(entryPoint);
        INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
        if (initialize && initialize(&controls)) return true;
        FreeLibrary(module);
        return false;
    }();
    return ready;
}

// Owner-drawn controls share caller-owned DCs during printing and WM_DRAWITEM.
// Restore the selected font as well as colors so DPI font changes can release
// the old font and one control cannot alter the next control's rendering.
class ScopedDcState final {
public:
    explicit ScopedDcState(HDC dc) : dc_(dc), saved_(SaveDC(dc)) {}
    ~ScopedDcState() { restore(); }
    void restore() { if (saved_ != 0) { RestoreDC(dc_, saved_); saved_ = 0; } }
    ScopedDcState(const ScopedDcState&) = delete;
    ScopedDcState& operator=(const ScopedDcState&) = delete;
private:
    HDC dc_;
    int saved_;
};

RECT monitorWorkArea(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    RECT area{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) return area;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

RECT fitWindowToWorkArea(RECT requested, const RECT& area) {
    const LONG workWidth = std::max<LONG>(1, area.right - area.left);
    const LONG workHeight = std::max<LONG>(1, area.bottom - area.top);
    const LONG width = std::clamp<LONG>(requested.right - requested.left, 1, workWidth);
    const LONG height = std::clamp<LONG>(requested.bottom - requested.top, 1, workHeight);
    const LONG left = std::clamp<LONG>(requested.left, area.left, area.right - width);
    const LONG top = std::clamp<LONG>(requested.top, area.top, area.bottom - height);
    return RECT{left, top, left + width, top + height};
}

std::wstring wideFromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return L"(invalid UTF-8)";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string utf8FromWide(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
bool isTemporalRestoration(const restoration::ModelPack& model) {
    return restoration::supportsOnnxCudaTemporalModel(model);
}

bool hasRestorationTask(const restoration::ModelPack& model, restoration::Task task) {
    return std::find(model.capabilities.tasks.begin(), model.capabilities.tasks.end(), task) != model.capabilities.tasks.end();
}

bool isUpscaleRestoration(const restoration::ModelPack& model) {
    return !isTemporalRestoration(model) && hasRestorationTask(model, restoration::Task::superResolution) &&
        restoration::supportsOnnxCudaOutputRateModel(model);
}

std::wstring restorationSelectorLabel(const restoration::ModelPack& model) {
    const std::wstring name = wideFromUtf8(model.displayName.empty() ? model.id : model.displayName);
    if (restoration::supportsStdfCudaModel(model)) return L"STDF-R3 · 压缩修复（本机评估）";
    if (isTemporalRestoration(model)) return L"FastDVDnet · 降噪（默认固定 σ25）";
    if (hasRestorationTask(model, restoration::Task::deblur)) return name + L"  ·  去模糊";
    if (isUpscaleRestoration(model))
        return model.id.starts_with("realesrgan-") ? L"超分辨率 ×2 · Real-ESRGAN" : name + L"  ·  超分辨率 ×2";
    return name + L"  ·  可选画质处理";
}

std::wstring processingStageLabel(std::string_view stage) {
    if (stage == "waiting_for_engine") return L"等待推理引擎";
    if (stage == "checking_source") return L"检查输入视频";
    if (stage == "loading_model") return L"加载 RIFE 模型（首次可能需要较长时间）";
    if (stage == "loading_restoration_model") return L"加载可选画质处理模型（首次可能需要较长时间）";
    if (stage == "decoding") return L"解码输入视频";
    if (stage == "restoration") return L"应用可选画质处理";
    if (stage == "processing") return L"插帧与编码";
    if (stage == "quality") return L"质量保护检查";
    if (stage == "resuming") return L"恢复已验证片段";
    if (stage == "validating") return L"验证输出文件";
    if (stage == "verifying_output") return L"校验最终输出";
    if (stage == "encoder_fallback") return L"编码器回退";
    if (stage == "paused") return L"已暂停";
    if (stage == "resumed") return L"继续处理";
    if (stage == "completed") return L"处理完成";
    return wideFromUtf8(stage);
}
#endif

std::wstring shorten(const std::wstring& value, std::size_t limit = 110) {
    if (value.size() <= limit) return value;
    return value.substr(0, limit / 2) + L"…" + value.substr(value.size() - limit / 2);
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
std::wstring prettyBytes(std::size_t bytes) {
    const double value = static_cast<double>(bytes);
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(1);
    if (bytes >= 1024ULL * 1024 * 1024) stream << (value / (1024.0 * 1024.0 * 1024.0)) << L" GiB 总显存";
    else stream << (value / (1024.0 * 1024.0)) << L" MiB 总显存";
    return stream.str();
}

std::wstring taskStateLabel(TaskState state) {
    switch (state) {
    case TaskState::queued: return L"排队中";
    case TaskState::running: return L"处理中";
    case TaskState::paused: return L"已暂停";
    case TaskState::interrupted: return L"已中断";
    case TaskState::failed: return L"失败";
    case TaskState::cancelled: return L"已取消";
    case TaskState::completed: return L"已完成";
    }
    return L"未知";
}

std::wstring taskHistoryText(const ConfiguredTask& configured, bool checkpointInspectionAvailable,
                             bool hasCommittedCheckpointSegments) {
    const auto& task = configured.task;
    std::wstring text;
    text.reserve(1800);
    text += L"任务状态：" + taskStateLabel(task.state) + L"\r\n";
    text += L"ID：" + wideFromUtf8(task.id) + L"\r\n";
    text += L"源文件：" + wideFromUtf8(task.sourcePath) + L"\r\n";
    text += L"输出文件：" + wideFromUtf8(task.outputPath) + L"\r\n";
    if (!task.createdAt.empty()) text += L"创建：" + wideFromUtf8(task.createdAt) + L"\r\n";
    if (!task.updatedAt.empty()) text += L"更新：" + wideFromUtf8(task.updatedAt) + L"\r\n";
    if (!task.error.empty()) text += L"失败原因：" + wideFromUtf8(task.error) + L"\r\n";
    if (task.state == TaskState::interrupted) {
        if (!checkpointInspectionAvailable)
            text += L"断点状态无法读取；刷新后重试，或使用“从头另存重做”。\r\n";
        else
            text += hasCommittedCheckpointSegments
                ? L"断点候选：已提交片段存在；继续时仍会校验文件和任务身份。\r\n"
                : L"断点候选：没有已提交片段；请使用“从头另存重做”。\r\n";
    }

    const bool completed = task.state == TaskState::completed;
    const auto& serialized = completed ? configured.result : configured.progress;
    if (serialized.empty()) {
        if (completed) text += L"\r\n没有可显示的输出摘要（可能是旧版任务记录）。\r\n";
        return text;
    }
    if (serialized.size() > 64U * 1024U) {
        text += L"\r\n任务摘要超过支持的大小限制；基础任务记录仍可用。\r\n";
        return text;
    }

    try {
        const auto summary = nlohmann::json::parse(serialized);
        if (!summary.is_object()) throw std::invalid_argument("task summary is not an object");
        const auto count = [](const nlohmann::json& object, const char* key) -> std::optional<std::uint64_t> {
            const auto found = object.find(key);
            if (found == object.end()) return {};
            if (found->is_number_unsigned()) return found->get<std::uint64_t>();
            if (found->is_number_integer()) {
                const auto value = found->get<std::int64_t>();
                if (value >= 0) return static_cast<std::uint64_t>(value);
            }
            return {};
        };
        const auto number = [&](const char* key) -> std::optional<double> {
            const auto found = summary.find(key);
            if (found == summary.end() || !found->is_number()) return {};
            const double value = found->get<double>();
            return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
        };
        const auto string = [&](const char* key) -> std::optional<std::string> {
            const auto found = summary.find(key);
            if (found == summary.end() || !found->is_string()) return {};
            return found->get<std::string>();
        };
        const auto appendCount = [&](const char* key, const wchar_t* label) {
            if (const auto value = count(summary, key)) text += std::wstring(label) + std::to_wstring(*value) + L"\r\n";
        };
        const auto appendNumber = [&](const char* key, const wchar_t* label, int precision) {
            const auto value = number(key);
            if (!value) return;
            std::wostringstream formatted;
            formatted << std::fixed << std::setprecision(precision) << *value;
            text += std::wstring(label) + formatted.str() + L"\r\n";
        };
        const auto appendString = [&](const char* key, const wchar_t* label) {
            if (const auto value = string(key); value && !value->empty())
                text += std::wstring(label) + wideFromUtf8(*value) + L"\r\n";
        };

        text += completed ? L"\r\n处理结果摘要：\r\n" : L"\r\n最近进度摘要：\r\n";
        if (completed) {
            appendCount("sourceFrames", L"源视频帧数：");
            appendCount("outputFrames", L"输出帧数：");
            appendCount("aiFrames", L"RIFE 生成帧数：");
            appendCount("originalFrames", L"原片保留帧数：");
            appendCount("heldFrames", L"质量保护保持帧数：");
            appendCount("processedFrames", L"本次实际处理帧数：");
            appendCount("reusedFrames", L"断点复用帧数：");
            appendCount("inferenceCalls", L"RIFE 推理调用次数：");
            appendCount("sceneCuts", L"镜头切换数：");
            appendNumber("elapsedSeconds", L"总耗时：", 1);
            appendNumber("processingFps", L"本次处理速度：", 1);
            appendString("modelId", L"插帧模型：");
            appendString("backend", L"推理后端：");
            appendString("encoder", L"编码器：");
            if (const auto validation = summary.find("validation"); validation != summary.end() && validation->is_object()) {
                const auto width = count(*validation, "width");
                const auto height = count(*validation, "height");
                if (width && height) text += L"输出尺寸：" + std::to_wstring(*width) + L" × " + std::to_wstring(*height) + L"\r\n";
            }
        } else {
            if (const auto stage = string("stage")) text += L"最近阶段：" + processingStageLabel(*stage) + L"\r\n";
            appendCount("frames", L"目标时间轴帧数：");
            appendCount("processedFrames", L"本次已处理帧数：");
            appendCount("reusedFrames", L"断点复用帧数：");
            appendCount("aiFrames", L"RIFE 生成帧数：");
            appendNumber("processingFps", L"本次处理速度：", 1);
            appendString("timestamp", L"最近媒体时间点：");
            const auto frames = count(summary, "frames");
            if (frames && *frames != 0) {
                const double processed = static_cast<double>(count(summary, "processedFrames").value_or(0));
                const double reused = static_cast<double>(count(summary, "reusedFrames").value_or(0));
                const double percent = std::clamp((processed + reused) * 100.0 / static_cast<double>(*frames), 0.0, 100.0);
                std::wostringstream formatted;
                formatted << std::fixed << std::setprecision(1) << percent;
                text += L"估算进度：" + formatted.str() + L"%\r\n";
            }
        }
    } catch (const std::exception&) {
        text += L"\r\n任务摘要无法读取；基础任务记录仍可用。\r\n";
    }
    return text;
}

std::wstring taskHistoryRowLabel(const ConfiguredTask& configured) {
    const auto& task = configured.task;
    const auto source = std::filesystem::path(wideFromUtf8(task.sourcePath)).filename().wstring();
    const auto output = std::filesystem::path(wideFromUtf8(task.outputPath)).filename().wstring();
    return L"[" + taskStateLabel(task.state) + L"]  " + (source.empty() ? L"(无源文件名)" : source) +
        L"  →  " + (output.empty() ? L"(无输出文件名)" : output);
}

std::filesystem::path restartedOutputPath(const ConfiguredTask& selected,
                                          const std::vector<ConfiguredTask>& tasks) {
    const std::filesystem::path original(wideFromUtf8(selected.task.outputPath));
    if (original.empty() || original.filename().empty())
        throw std::invalid_argument("Interrupted task has no usable output path");
    const auto normalizedAbsolute = [](const std::filesystem::path& value) {
        return std::filesystem::absolute(value).lexically_normal().wstring();
    };
    for (std::uint32_t index = 1; index <= 10000; ++index) {
        const std::wstring suffix = index == 1 ? L" - 重做" : L" - 重做 (" + std::to_wstring(index) + L")";
        const auto candidate = original.parent_path() /
            (original.stem().wstring() + suffix + original.extension().wstring());
        std::error_code error;
        if (std::filesystem::exists(candidate, error)) continue;
        if (error) throw std::filesystem::filesystem_error("Could not inspect restart output", candidate, error);
        const auto absoluteCandidate = normalizedAbsolute(candidate);
        const bool alreadyAssigned = std::any_of(tasks.begin(), tasks.end(), [&](const ConfiguredTask& task) {
            const auto other = normalizedAbsolute(std::filesystem::path(wideFromUtf8(task.task.outputPath)));
            return CompareStringOrdinal(absoluteCandidate.c_str(), -1, other.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!alreadyAssigned) return candidate;
    }
    throw std::runtime_error("Could not find an unused output path for restarting this task");
}
#endif

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
std::wstring rationalText(const Rational& value) { return wideFromUtf8(value.toString()); }
std::wstring streamSummary(const media::MediaInfo& info) {
    const auto selected = std::find_if(info.streams.begin(), info.streams.end(), [&](const auto& stream) {
        return stream.index == info.preferredVideoStream && stream.type == "video";
    });
    if (selected == info.streams.end()) return L"没有找到可用的视频流";
    std::wstring result = std::to_wstring(selected->width) + L" × " + std::to_wstring(selected->height) + L"  ·  ";
    result += selected->averageRate ? rationalText(*selected->averageRate) : L"未知";
    result += L" FPS  ·  " + wideFromUtf8(selected->codec);
    if (selected->color.bitDepth > 0) result += L"  ·  " + std::to_wstring(selected->color.bitDepth) + L"-bit";
    return result;
}

#endif

int parsePositiveInteger(std::wstring_view value, const char* field) {
    const auto text = utf8FromWide(value);
    if (text.empty()) throw std::invalid_argument(std::string(field) + " must not be empty");
    int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument(std::string(field) + " must be an integer");
    return parsed;
}

}

NativeApp::NativeApp(std::filesystem::path applicationDirectory, std::filesystem::path localAppDataRoot) {
    instance_ = GetModuleHandleW(nullptr);
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(instance_, modulePath, static_cast<DWORD>(std::size(modulePath)));
    applicationDirectory_ = applicationDirectory.empty()
        ? (length ? std::filesystem::path(modulePath).parent_path() : std::filesystem::current_path())
        : std::filesystem::absolute(applicationDirectory).lexically_normal();
    backgroundBrush_ = CreateSolidBrush(RGB(12, 15, 21));
    editBrush_ = CreateSolidBrush(RGB(20, 25, 34));
    staticBrush_ = CreateSolidBrush(RGB(19, 24, 32));
    state_.processingAvailable = false;
    state_.status = L"选择本地视频开始检查";
    platformSummary_ = L"本机高帧率母版：保留自定义帧率和 MKV 输出";
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    processingService_ = std::make_unique<application::ProcessingService>();
    state_.processingAvailable = true;
    try {
        const auto root = localAppDataRoot.empty() ? nativeLocalAppDataRoot() : std::filesystem::absolute(localAppDataRoot).lexically_normal();
        dataDirectory_ = prepareNativeDataDirectory(applicationDirectory_, root);
    } catch (const std::exception&) {
        dataDirectory_.notice = L"无法确定安全的本机用户数据目录；队列和模型设置已停用，原数据保留。";
    }
    try {
        if (!dataDirectory_.writable || !dataDirectory_.preferencesReady)
            throw std::runtime_error("Native model data directory is unavailable");
        (void)checkedNativeModelDirectory(dataDirectory_.directory, false);
        const auto settings = dataDirectory_.directory / L"local-models.txt";
        const DWORD attributes = GetFileAttributesW(settings.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Model preferences must not be a reparse point");
        modelPreferences_ = loadNativeModelPreferences(settings);
    } catch (const std::exception&) {
        modelPreferencesWritable_ = false;
        modelCenterNotice_ = L"模型设置读取或迁移失败；为保护原数据，本次禁止更改登记和默认项。请查看详情中的数据目录。";
    }
    discoverLocalResources();
    try {
        if (!dataDirectory_.writable || !dataDirectory_.queueReady)
            throw std::runtime_error("Native queue directory or migration is unavailable; old data was preserved");
        taskQueue_ = std::make_unique<application::TaskQueue>(dataDirectory_.directory / L"queue.sqlite");
        const auto recovered = taskQueue_->recoveredCount();
        const auto published = taskQueue_->recoveredPublishedOutputCount();
        if (recovered != 0 || published != 0) {
            queueSummary_ = L"本地队列已恢复：" + std::to_wstring(recovered) + L" 个中断任务";
            if (published != 0) queueSummary_ += L"，采纳 " + std::to_wstring(published) + L" 个已验证输出";
        } else {
            queueSummary_ = L"本地队列已就绪";
        }
    } catch (const std::exception& error) {
        queueSummary_ = L"本地队列不可用：" + wideFromUtf8(error.what());
        taskQueue_.reset();
    }
#else
    (void)localAppDataRoot;
    state_.status = L"当前构建未启用完整媒体和 ONNX 服务";
#endif
}

NativeApp::~NativeApp() {
    closing_ = true;
    callbackWindow_.store(nullptr);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    comparisonWindow_.reset();
    modelInstallThread_.request_stop();
    if (modelInstallThread_.joinable()) modelInstallThread_.join();
    if (modelCenterWindow_ && IsWindow(modelCenterWindow_)) DestroyWindow(modelCenterWindow_);
    if (modelCenterFont_) DeleteObject(modelCenterFont_);
    if (modelCenterHeadingFont_) DeleteObject(modelCenterHeadingFont_);
    if (historyWindow_ && IsWindow(historyWindow_)) DestroyWindow(historyWindow_);
    historyThread_.request_stop();
    if (historyThread_.joinable()) historyThread_.join();
    queueImportStop_.request_stop();
    if (taskQueue_) taskQueue_->stop();
    if (queueOwnerThread_.joinable()) queueOwnerThread_.join();
    taskQueue_.reset();
    if (processingService_) processingService_->cancel();
    processingService_.reset();
#endif
    if (font_) DeleteObject(font_);
    if (backgroundBrush_) DeleteObject(backgroundBrush_);
    if (editBrush_) DeleteObject(editBrush_);
    if (staticBrush_) DeleteObject(staticBrush_);
}

bool NativeApp::create(HINSTANCE instance, int showCommand) {
    instance_ = instance ? instance : GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = &NativeApp::windowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = backgroundBrush_;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const UINT systemDpi = GetDpiForSystem();
    const float scale = static_cast<float>(systemDpi ? systemDpi : 96U) / 96.0F;
    RECT desired{0, 0, scaled(1366, scale), scaled(768, scale)};
    const DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL;
    if (!AdjustWindowRectExForDpi(&desired, windowStyle, FALSE, WS_EX_APPWINDOW, systemDpi ? systemDpi : 96U))
        AdjustWindowRectEx(&desired, windowStyle, FALSE, WS_EX_APPWINDOW);
    const RECT work = monitorWorkArea(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const LONG width = std::min(desired.right - desired.left, work.right - work.left);
    const LONG height = std::min(desired.bottom - desired.top, work.bottom - work.top);
    const RECT initial = fitWindowToWorkArea(RECT{work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2,
        work.left + (work.right - work.left - width) / 2 + width,
        work.top + (work.bottom - work.top - height) / 2 + height}, work);
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle,
        windowStyle, initial.left, initial.top, initial.right - initial.left, initial.bottom - initial.top,
        nullptr, nullptr, instance_, this);
    if (!window_) return false;
    // Keep the standard caption buttons and keyboard behavior while matching
    // the app's dark visual surface on Windows 10/11. Older builds simply
    // ignore these optional DWM attributes.
    const BOOL darkCaption = TRUE;
    const COLORREF captionColor = RGB(11, 16, 24);
    const COLORREF captionTextColor = RGB(242, 247, 255);
    DwmSetWindowAttribute(window_, 20, &darkCaption, sizeof(darkCaption));
    DwmSetWindowAttribute(window_, 35, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(window_, 36, &captionTextColor, sizeof(captionTextColor));
    callbackWindow_.store(window_);
    DragAcceptFiles(window_, TRUE);
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

int NativeApp::run() {
    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) return result == 0 ? static_cast<int>(message.wParam) : 1;
        if (statusWindow_.handleDialogMessage(message)) continue;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        if (comparisonWindow_ && comparisonWindow_->handleDialogMessage(message)) continue;
        if (modelCenterWindow_ && IsDialogMessageW(modelCenterWindow_, &message)) continue;
#endif
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB &&
            (message.hwnd == window_ || IsChild(window_, message.hwnd)) && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            if (focusNextControl((GetKeyState(VK_SHIFT) & 0x8000) != 0)) continue;
        }
        if (!IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

float NativeApp::dpiScale() const {
    const UINT dpi = window_ ? GetDpiForWindow(window_) : 96U;
    return static_cast<float>(dpi ? dpi : 96U) / 96.0F;
}

std::wstring NativeApp::controlText(HWND control) const {
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) GetWindowTextW(control, result.data(), length + 1);
    return result;
}

std::filesystem::path NativeApp::currentSource() const {
    return std::filesystem::path(controlText(sourceEdit_));
}

std::filesystem::path NativeApp::currentOutput() const {
    const auto text = controlText(outputEdit_);
    if (!outputPath_.empty() && text == outputPath_.wstring()) return outputPath_;
    return std::filesystem::path(text);
}

std::filesystem::path NativeApp::pathForEdit(HWND edit) const {
    if (edit == sourceEdit_) return currentSource();
    if (edit == outputEdit_) return currentOutput();
    return {};
}

void NativeApp::showFullPath(HWND edit) {
    const auto path = pathForEdit(edit);
    const auto value = path.empty() ? L"（尚未设置路径）" : path.wstring();
    const auto title = edit == outputEdit_ ? L"完整输出路径" : L"完整源视频路径";
    MessageBoxW(window_, value.c_str(), title, MB_OK | MB_ICONINFORMATION);
}

void NativeApp::showPathContextMenu(HWND edit, POINT screenPoint) {
    if (!edit || (edit != sourceEdit_ && edit != outputEdit_)) return;
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        RECT rect{};
        GetWindowRect(edit, &rect);
        screenPoint.x = rect.left + (rect.right - rect.left) / 2;
        screenPoint.y = rect.top + (rect.bottom - rect.top) / 2;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kPathMenuView, L"查看完整路径");
    AppendMenuW(menu, MF_STRING, kPathMenuCopy, L"复制完整路径");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kPathMenuCopySelection, L"复制所选文本");
    AppendMenuW(menu, MF_STRING, kPathMenuCut, L"剪切");
    AppendMenuW(menu, MF_STRING, kPathMenuPaste, L"粘贴");
    AppendMenuW(menu, MF_STRING, kPathMenuSelectAll, L"全选");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case kPathMenuView:
        showFullPath(edit);
        break;
    case kPathMenuCopy: {
        const auto path = pathForEdit(edit).wstring();
        if (path.empty() || !OpenClipboard(window_)) break;
        EmptyClipboard();
        const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                std::memcpy(target, path.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            } else GlobalFree(memory);
        }
        CloseClipboard();
        break;
    }
    case kPathMenuCopySelection:
        SendMessageW(edit, WM_COPY, 0, 0);
        break;
    case kPathMenuCut:
        SendMessageW(edit, WM_CUT, 0, 0);
        break;
    case kPathMenuPaste:
        SendMessageW(edit, WM_PASTE, 0, 0);
        break;
    case kPathMenuSelectAll:
        SendMessageW(edit, EM_SETSEL, 0, -1);
        break;
    default:
        break;
    }
}

void NativeApp::subclassPathEdit(HWND edit) {
    if (!edit) return;
    const LONG_PTR original = SetWindowLongPtrW(edit, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&NativeApp::pathEditProcedure));
    SetPropW(edit, kPathEditOwnerProperty, reinterpret_cast<HANDLE>(this));
    SetPropW(edit, kPathEditOriginalProcProperty, reinterpret_cast<HANDLE>(original));
    if (!ensureTooltipControls()) return;
    const HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    if (!tooltip) return;
    auto text = std::make_unique<std::wstring>();
    if (!SetPropW(edit, kPathTooltipProperty, tooltip) ||
        !SetPropW(edit, kPathTooltipTextProperty, text.get())) {
        RemovePropW(edit, kPathTooltipProperty);
        RemovePropW(edit, kPathTooltipTextProperty);
        DestroyWindow(tooltip);
        return;
    }
    TOOLINFOW tool{};
    // The executable does not require a v6 common-controls manifest. Use the
    // v2 ABI so the system v5 tooltip accepts the tool instead of rejecting it
    // (and destroying its HWND below) for the v6-only lpReserved field.
    tool.cbSize = TTTOOLINFOW_V2_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = window_;
    tool.uId = reinterpret_cast<UINT_PTR>(edit);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
        RemovePropW(edit, kPathTooltipProperty);
        RemovePropW(edit, kPathTooltipTextProperty);
        DestroyWindow(tooltip);
        return;
    }
    text.release();
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, scaled(520, dpiScale()));
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
}

LRESULT CALLBACK NativeApp::pathEditProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<NativeApp*>(GetPropW(window, kPathEditOwnerProperty));
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kPathEditOriginalProcProperty));
    if (app && message == WM_CONTEXTMENU) {
        POINT point{
            static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
            static_cast<LONG>(static_cast<short>(HIWORD(lParam)))
        };
        app->showPathContextMenu(window, point);
        return 0;
    }
    if (app && message == WM_SETTEXT) {
        // Long filesystem paths are much more useful when the selected file
        // name is visible immediately. Keep the full text in the edit control
        // and scroll its caret to the end after every programmatic update.
        const LRESULT result = original
            ? CallWindowProcW(original, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
        if (result) {
            const auto length = SendMessageW(window, WM_GETTEXTLENGTH, 0, 0);
            SendMessageW(window, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
            SendMessageW(window, EM_SCROLLCARET, 0, 0);
        }
        return result;
    }
    if (message == WM_NCDESTROY) {
        const auto tooltip = reinterpret_cast<HWND>(RemovePropW(window, kPathTooltipProperty));
        if (tooltip && IsWindow(tooltip)) DestroyWindow(tooltip);
        delete reinterpret_cast<std::wstring*>(RemovePropW(window, kPathTooltipTextProperty));
        RemovePropW(window, kPathEditOwnerProperty);
        RemovePropW(window, kPathEditOriginalProcProperty);
    }
    return original ? CallWindowProcW(original, window, message, wParam, lParam)
                     : DefWindowProcW(window, message, wParam, lParam);
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
bool NativeApp::hasBatchSelection() const noexcept {
    if (queuedInputs_.size() > 1) return true;
    if (queuedInputs_.empty()) return false;
    std::error_code error;
    return std::filesystem::is_directory(queuedInputs_.front(), error);
}
#endif

NativeApp::OutputTargetCheck NativeApp::outputTargetCheck(int selection) const {
    return nativeOutputTarget(sourceWidth_, sourceHeight_, selection);
}

std::wstring NativeApp::outputTargetLabel(int selection) const {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (selection > 0 && hasBatchSelection())
        return std::wstring(selection == 1 ? L"1440p" : L"2160p") + L"  ·  各视频按源比例计算";
#endif
    const auto target = outputTargetCheck(selection);
    if (!target.selected) return L"原始输出尺寸";
    if (target.width <= 0 || target.height <= 0)
        return std::wstring(selection == 1 ? L"1440p" : L"2160p") + L"  ·  导入视频后按源比例计算";

    std::wstring label = target.nominalSize ? L"" : L"约 ";
    label += selection == 1 ? L"1440p  ·  " : L"2160p  ·  ";
    label += std::to_wstring(target.width) + L" × " + std::to_wstring(target.height);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (sourceInspected_) {
        if (!target.dimensionsSupported) label += L"  ·  超出编码尺寸";
        else if (!target.aspectMatches) label += L"  ·  源比例不匹配";
        else if (!target.generatedEnough) label += L"  ·  源尺寸不足";
        else if (!target.dimensionsEven) label += L"  ·  需要偶数尺寸";
        else label += L"  ·  保持源比例";
    }
#endif
    return label;
}

NativeApp::StartReadiness NativeApp::evaluateStartReadiness() const {
    StartReadiness result{};
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    std::vector<std::wstring> blockers;
    const auto add = [&](std::wstring text) {
        if (!text.empty()) blockers.emplace_back(std::move(text));
    };
    const auto source = currentSource();
    if (source.empty()) {
        result.status = L"选择源视频开始检查";
        return result;
    }
    const bool batch = hasBatchSelection();
    if (!batch && !sourceInspected_) {
        std::error_code sourceError;
        result.status = std::filesystem::is_regular_file(source, sourceError)
            ? L"正在检查视频和本机条件…"
            : L"源文件不存在或无法读取，请重新选择";
        return result;
    }
    if (!batch && !sourceColorSupported_) add(L"色彩配置");
    const auto output = currentOutput();
    const bool exportYuv420p = media::automaticOutputPixelFormat(utf8(output)) == "yuv420p";
    if (!batch && sourceInspected_ && exportYuv420p && sourceColor_.bitDepth > 8)
        add(L"10 位源片不能转为 8 位输出；可尝试 MKV 自动编码");
    const auto& capabilities = exportYuv420p ? encoderCapabilitiesYuv420p_ : encoderCapabilities_;
    bool selectedEncoderAvailable = encoderAvailable_;
    const int encoderIndex = encoderCombo_ ? static_cast<int>(SendMessageW(encoderCombo_, CB_GETCURSEL, 0, 0)) : 0;
    if (encoderIndex == 0) {
        selectedEncoderAvailable = false;
        for (const auto& capability : capabilities) {
            if (media::isAutomaticEncoderCandidate(utf8(output), capability.encoder) && capability.contextOpened) {
                selectedEncoderAvailable = true;
                break;
            }
        }
    } else if (encoderIndex == 1 || encoderIndex == 2) {
        const char* selectedEncoder = encoderIndex == 1 ? "h264_nvenc" : "libopenh264";
        const auto capability = std::find_if(capabilities.begin(), capabilities.end(), [&](const auto& item) {
            return item.encoder == selectedEncoder;
        });
        selectedEncoderAvailable = capability != capabilities.end() && capability->contextOpened;
    }
    if (!batch && !selectedEncoderAvailable) add(L"视频编码器");
    if (!batch && !deviceAvailable_) add(L"CUDA 设备");
    if (!modelPack_) add(L"RIFE 模型");
    if (!state_.runtimeReady) add(L"推理运行库");

    std::error_code outputError;
    if (output.empty()) add(L"输出位置");
    else if (batch) {
        if (!std::filesystem::is_directory(output, outputError)) add(L"输出目录");
    } else if (std::filesystem::exists(output, outputError)) add(L"输出文件已存在");

    const auto& rates = kTargetRates;
    const int rateIndex = rateCombo_ ? static_cast<int>(SendMessageW(rateCombo_, CB_GETCURSEL, 0, 0)) : -1;
    try {
        Rational rate(60);
        if (rateIndex >= 0 && rateIndex < static_cast<int>(rates.size())) rate = Rational(rates[static_cast<std::size_t>(rateIndex)]);
        else if (rateIndex == static_cast<int>(rates.size())) rate = Rational::parse(utf8FromWide(controlText(rateCustomEdit_)));
        if (rate <= Rational(0)) throw std::invalid_argument("target frame rate must be positive");
        if (platformCombo_ && SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0) > 0 &&
            rate.denominator() == 1 && rate.numerator() > std::numeric_limits<int>::max() / 2)
            throw std::invalid_argument("target frame rate exceeds the platform GOP limit");
    } catch (...) {
        add(L"目标帧率");
    }
    if (motionBlurCheck_ && SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        try {
            const int shutter = parsePositiveInteger(controlText(shutterEdit_), "shutter angle");
            const int samples = parsePositiveInteger(controlText(samplesEdit_), "sample count");
            if (shutter < 0 || shutter > 360 || samples < 1 || samples > 16) throw std::invalid_argument("invalid motion blur settings");
        } catch (...) {
            add(L"动态曝光参数");
        }
    }
    if (automaticTemporalDenoise_) {
        try { (void)nativeRelativeAdjustment(controlText(denoiseAdjustmentEdit_)); }
        catch (const std::exception&) { add(L"自动降噪相对偏移（−100%…+100%）"); }
    }
    if (automaticCompressionRestoration_) {
        try { (void)nativeRelativeAdjustment(controlText(compressionAdjustmentEdit_)); }
        catch (const std::exception&) { add(L"自动压缩相对偏移（−100%…+100%）"); }
    }
    if (automaticDetailRestoration_) {
        try { (void)nativeRelativeAdjustment(controlText(detailAdjustmentEdit_)); }
        catch (const std::exception&) { add(L"自动细节相对偏移（−100%…+100%）"); }
        if (motionBlurCheck_ && SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            try {
                if (parsePositiveInteger(controlText(shutterEdit_), "shutter angle") > 0 &&
                    parsePositiveInteger(controlText(samplesEdit_), "sample count") > 1)
                    add(L"自动细节与自然动态曝光冲突，请关闭其中一项");
            } catch (const std::exception&) { /* Invalid exposure is reported above. */ }
        }
    }
    const int temporalRestorationIndex = temporalRestorationCombo_ ? static_cast<int>(SendMessageW(temporalRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int deblurRestorationIndex = deblurRestorationCombo_ ? static_cast<int>(SendMessageW(deblurRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int restorationIndex = restorationCombo_ ? static_cast<int>(SendMessageW(restorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int outputSizeIndex = outputSizeCombo_ ? static_cast<int>(SendMessageW(outputSizeCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int compressionIndex = compressionRestorationCombo_ ? static_cast<int>(SendMessageW(compressionRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int compressionStrengthIndex = compressionStrengthCombo_ ? static_cast<int>(SendMessageW(compressionStrengthCombo_, CB_GETCURSEL, 0, 0)) : 0;
    if (compressionStrengthIndex < 0 || compressionStrengthIndex > 3) add(L"压缩修复强度无效");
    else if (compressionStrengthIndex > 0) {
        if (compressionIndex <= 0 || compressionIndex > static_cast<int>(compressionRestorationModels_.size()) ||
            !restoration::supportsStdfCudaModel(compressionRestorationModels_[static_cast<std::size_t>(compressionIndex - 1)]))
            add(L"已验证的 STDF CUDA 压缩修复模型");
        if (!batch && sourceInspected_) {
            const bool tiled = compressionTilingCheck_ && SendMessageW(compressionTilingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const auto issue = nativeCompressionSourceIssue(sourceWidth_, sourceHeight_, sourceColor_, tiled);
            if (!issue.empty()) add(issue);
        }
    }
    if (!batch && sourceInspected_ && sourceColorSupported_) {
        if (temporalRestorationIndex > 0 && temporalRestorationIndex <= static_cast<int>(temporalRestorationModels_.size())) {
            const auto& profiles = temporalRestorationModels_[static_cast<std::size_t>(temporalRestorationIndex - 1)].capabilities.supportedInputProfiles;
            if (std::find(profiles.begin(), profiles.end(), sourceInputProfile_) == profiles.end())
                add(L"所选可选降噪模型不支持该视频的色彩配置");
        }
        if (restorationIndex > 0 && restorationIndex <= static_cast<int>(spatialRestorationModels_.size())) {
            const auto& profiles = spatialRestorationModels_[static_cast<std::size_t>(restorationIndex - 1)].capabilities.supportedInputProfiles;
            if (std::find(profiles.begin(), profiles.end(), sourceInputProfile_) == profiles.end())
                add(L"所选超分模型不支持该视频的色彩配置");
        }
        if (deblurRestorationIndex > 0 && deblurRestorationIndex <= static_cast<int>(deblurRestorationModels_.size())) {
            const auto& profiles = deblurRestorationModels_[static_cast<std::size_t>(deblurRestorationIndex - 1)].capabilities.supportedInputProfiles;
            if (std::find(profiles.begin(), profiles.end(), sourceInputProfile_) == profiles.end())
                add(L"所选去模糊模型仅支持已验证的 SDR8 色彩配置");
        }
    }
    if (outputSizeIndex > 0) {
        const bool hasModel = restorationIndex > 0 && restorationIndex <= static_cast<int>(spatialRestorationModels_.size());
        const bool supportsFixedSize = hasModel && isUpscaleRestoration(spatialRestorationModels_[static_cast<std::size_t>(restorationIndex - 1)]);
        const auto target = outputTargetCheck(outputSizeIndex);
        if (!supportsFixedSize) add(L"当前可选画质处理模型不支持固定输出尺寸");
        else if (!batch && !target.dimensionsSupported) add(L"目标尺寸超出编码限制");
        else if (!batch && !target.aspectMatches) add(L"输入比例不匹配");
        else if (!batch && !target.generatedEnough) add(L"目标尺寸超出 x2 能力");
        else if (!batch && !target.dimensionsEven) add(L"目标尺寸需要偶数像素");
    }
    if (taskQueue_ == nullptr) add(L"本地任务队列");
    if (queueSessionActive_.load()) add(L"当前任务正在运行");
    if (processingService_ && processingService_->busy()) add(L"当前任务正在运行");
    if (closing_) add(L"正在关闭");

    result.ready = blockers.empty();
    if (result.ready) {
        result.status = !batch && sourceInspected_ && exportYuv420p && sourceColor_.pixelFormat != "yuv420p"
            ? L"检查完成；输出将转为 8 位 4:2:0" : L"检查完成，可以开始";
    }
    else {
        result.status = L"还需设置：";
        for (std::size_t i = 0; i < blockers.size(); ++i) {
            if (i != 0) result.status += L"、";
            result.status += blockers[i];
        }
    }
#else
    result.status = L"当前构建未启用完整媒体和 ONNX 服务";
#endif
    return result;
}

void NativeApp::refreshReadinessStatus() {
    if (!state_.processing) {
        state_.stage.clear();
        state_.status = evaluateStartReadiness().status;
    }
}

std::string NativeApp::utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::wstring NativeApp::pathText(const std::filesystem::path& path) {
    if (path.empty()) return {};
    const auto name = path.filename().wstring();
    if (name.empty()) return shorten(path.wstring(), 80);
    const auto parent = path.parent_path().wstring();
    return shorten(name, 64) + (parent.empty() ? L"" : L"  ·  " + shorten(parent, 48));
}

void NativeApp::setOutputPath(const std::filesystem::path& output) {
    outputPath_ = output;
    // Keep the complete value in the single-line editable control. The field
    // scrolls on narrow windows, while its summary starts with the filename
    // and its hover tooltip shows the full path. This keeps copy and editing
    // lossless instead of making the ellipsis the stored value.
    const auto value = output.wstring();
    SetWindowTextW(outputEdit_, value.c_str());
}

void NativeApp::createControls() {
    const auto createStatic = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    const auto createButton = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    sourceLabel_ = createStatic(L"源视频", 1101);
    sourceEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSourceEdit)), instance_, nullptr);
    SendMessageW(sourceEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"悬停、右键或 Shift+F10 查看完整路径"));
    sourceButton_ = createButton(L"＋ 导入视频", kSelectSource);
    sourceFolderButton_ = createButton(L"导入文件夹", kSelectSourceFolder);
    outputLabel_ = createStatic(L"输出文件", 1102);
    outputEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutputEdit)), instance_, nullptr);
    SendMessageW(outputEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"悬停、右键或 Shift+F10 查看完整路径"));
    subclassPathEdit(sourceEdit_);
    subclassPathEdit(outputEdit_);
    outputButton_ = createButton(L"选择位置", kSelectOutput);
    rateLabel_ = createStatic(L"目标帧率", 1103);
    rateCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRateCombo)), instance_, nullptr);
    rateCustomEdit_ = CreateWindowExW(0, L"EDIT", L"300/1", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRateCustomEdit)), instance_, nullptr);
    encoderLabel_ = createStatic(L"编码器", 1104);
    encoderCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEncoderCombo)), instance_, nullptr);
    platformCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPlatformCombo)), instance_, nullptr);
    modelLabel_ = createStatic(L"模型和运行库", 1105);
    platformLabel_ = createStatic(L"平台预设", 1108);
    modelButton_ = createButton(L"本地模型中心", kSelectModel);
    runtimeButton_ = createButton(L"选择运行库", kSelectRuntime);
    motionBlurCheck_ = CreateWindowExW(0, L"BUTTON", L"自然动态曝光",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMotionBlurCheck)), instance_, nullptr);
    shutterLabel_ = createStatic(L"快门角度", kShutterLabel);
    shutterEdit_ = CreateWindowExW(0, L"EDIT", L"180", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShutterEdit)), instance_, nullptr);
    samplesLabel_ = createStatic(L"采样数", kSamplesLabel);
    samplesEdit_ = CreateWindowExW(0, L"EDIT", L"4", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSamplesEdit)), instance_, nullptr);
    temporalRestorationLabel_ = createStatic(L"可选降噪", kTemporalRestorationLabel);
    temporalRestorationCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTemporalRestorationCombo)), instance_, nullptr);
    deblurRestorationCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeblurRestorationCombo)), instance_, nullptr);
    compressionRestorationCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompressionRestorationCombo)), instance_, nullptr);
    compressionStrengthCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompressionStrengthCombo)), instance_, nullptr);
    compressionTilingCheck_ = CreateWindowExW(0, L"BUTTON", L"高清分块（实验；较慢）",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompressionTilingCheck)), instance_, nullptr);
    restorationLabel_ = createStatic(L"超分辨率", kRestorationLabel);
    restorationCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRestorationCombo)), instance_, nullptr);
    outputSizeLabel_ = createStatic(L"目标尺寸", kOutputSizeLabel);
    outputSizeCombo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutputSizeCombo)), instance_, nullptr);
    for (const auto combo : {rateCombo_, encoderCombo_, platformCombo_, temporalRestorationCombo_, deblurRestorationCombo_, compressionRestorationCombo_, compressionStrengthCombo_, restorationCombo_, outputSizeCombo_}) styleCombo(combo);
    advancedButton_ = createButton(L"参数  ˅", kAdvancedToggle);
    qualityDisclosureButton_ = createButton(L"展开画质设置  ▾", kQualityDisclosure);
    startButton_ = createButton(L"开始插帧", kStart);
    pauseResumeButton_ = createButton(L"暂停", kPauseResume);
    cancelButton_ = createButton(L"取消", kCancel);
    rejectionDetailsButton_ = createButton(L"拒绝明细", kRejectionDetails);
    statusDetailsButton_ = createButton(L"查看详情", kStatusDetails);
    historyButton_ = createButton(L"任务历史", kTaskHistory);
    comparisonButton_ = createButton(L"逐帧比较", kComparison);
    automaticDenoiseButton_ = createButton(L"自动降噪：关", kAutomaticDenoise);
    denoiseAdjustmentEdit_ = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDenoiseAdjustmentEdit)), instance_, nullptr);
    SendMessageW(denoiseAdjustmentEdit_, EM_SETLIMITTEXT, 24, 0);
    SendMessageW(denoiseAdjustmentEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"-100 … +100；0 跟随自动"));
    denoiseAdjustmentResetButton_ = createButton(L"归零", kDenoiseAdjustmentReset);
    automaticDetailButton_ = createButton(L"自动细节：关", kAutomaticDetail);
    detailAdjustmentEdit_ = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailAdjustmentEdit)), instance_, nullptr);
    SendMessageW(detailAdjustmentEdit_, EM_SETLIMITTEXT, 24, 0);
    SendMessageW(detailAdjustmentEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"-100 … +100；0 跟随自动"));
    detailAdjustmentResetButton_ = createButton(L"归零", kDetailAdjustmentReset);
    automaticCompressionButton_ = createButton(L"自动压缩：关", kAutomaticCompression);
    compressionAdjustmentEdit_ = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCompressionAdjustmentEdit)), instance_, nullptr);
    SendMessageW(compressionAdjustmentEdit_, EM_SETLIMITTEXT, 24, 0);
    SendMessageW(compressionAdjustmentEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"-100 … +100；0 跟随自动"));
    compressionAdjustmentResetButton_ = createButton(L"归零", kCompressionAdjustmentReset);
    EnableWindow(denoiseAdjustmentEdit_, FALSE);
    EnableWindow(denoiseAdjustmentResetButton_, FALSE);
    EnableWindow(detailAdjustmentEdit_, FALSE);
    EnableWindow(detailAdjustmentResetButton_, FALSE);
    EnableWindow(compressionAdjustmentEdit_, FALSE);
    EnableWindow(compressionAdjustmentResetButton_, FALSE);
    ShowWindow(rejectionDetailsButton_, SW_HIDE);
#ifndef RIFELOVE_NATIVE_HAS_PROCESSING
    ShowWindow(historyButton_, SW_HIDE);
    EnableWindow(comparisonButton_, FALSE);
    EnableWindow(automaticDenoiseButton_, FALSE);
    EnableWindow(automaticDetailButton_, FALSE);
    EnableWindow(automaticCompressionButton_, FALSE);
#endif
    for (const auto checkbox : {motionBlurCheck_, compressionTilingCheck_}) {
        const auto checkboxProc = SetWindowLongPtrW(checkbox, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NativeApp::checkboxProcedure));
        SetPropW(checkbox, L"RifeLoveCheckboxProc", reinterpret_cast<HANDLE>(checkboxProc));
        SetPropW(checkbox, L"RifeLoveCheckboxOwner", reinterpret_cast<HANDLE>(this));
    }

    for (const wchar_t* value : {L"30 FPS", L"60 FPS", L"120 FPS", L"144 FPS", L"240 FPS", L"300 FPS", L"360 FPS", L"480 FPS", L"600 FPS", L"自定义有理数"}) SendMessageW(rateCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(rateCombo_, CB_SETCURSEL, 1, 0);
    ShowWindow(rateCustomEdit_, SW_HIDE);
    for (const wchar_t* value : {L"自动（NVENC 优先）", L"H.264 NVENC", L"软件 H.264"}) SendMessageW(encoderCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(encoderCombo_, CB_SETCURSEL, 0, 0);
    for (const wchar_t* value : {L"本机高帧率母版 / 自定义", L"YouTube · 60 FPS 本地副本", L"Bilibili · 60 FPS 本地副本", L"Bilibili · 120 FPS 历史实验", L"抖音 · 30 FPS 本地副本", L"抖音 · 60 FPS 本地副本"})
        SendMessageW(platformCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(platformCombo_, CB_SETCURSEL, 0, 0);
    SendMessageW(temporalRestorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"关闭（默认：不使用可选降噪）"));
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    for (const auto& model : temporalRestorationModels_) {
        const auto name = restorationSelectorLabel(model);
        SendMessageW(temporalRestorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
#endif
    SendMessageW(temporalRestorationCombo_, CB_SETCURSEL, 0, 0);
    SendMessageW(deblurRestorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"关闭（默认：不使用实验性去模糊）"));
    SendMessageW(deblurRestorationCombo_, CB_SETCURSEL, 0, 0);
    SendMessageW(compressionRestorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未选择压缩修复模型"));
    SendMessageW(compressionRestorationCombo_, CB_SETCURSEL, 0, 0);
    for (const wchar_t* value : {L"关闭", L"25%", L"50%", L"100%"})
        SendMessageW(compressionStrengthCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(compressionStrengthCombo_, CB_SETCURSEL, 0, 0);
    SendMessageW(restorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"关闭（默认：不使用超分辨率）"));
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    for (const auto& model : spatialRestorationModels_) {
        const auto name = restorationSelectorLabel(model);
        SendMessageW(restorationCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
#endif
    SendMessageW(restorationCombo_, CB_SETCURSEL, 0, 0);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    syncRestorationSelectors();
#else
    EnableWindow(modelButton_, FALSE);
    EnableWindow(deblurRestorationCombo_, FALSE);
    EnableWindow(compressionRestorationCombo_, FALSE);
    EnableWindow(compressionStrengthCombo_, FALSE);
    EnableWindow(compressionTilingCheck_, FALSE);
#endif
    for (const wchar_t* value : {L"默认：原尺寸 / 超分 ×2", L"1440p  ·  2560 × 1440", L"2160p  ·  3840 × 2160"})
        SendMessageW(outputSizeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(outputSizeCombo_, CB_SETCURSEL, 0, 0);
    SendMessageW(motionBlurCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
    ShowWindow(modelButton_, SW_HIDE);
    ShowWindow(runtimeButton_, SW_HIDE);
    ShowWindow(modelLabel_, SW_HIDE);
    ShowWindow(motionBlurCheck_, SW_HIDE);
    ShowWindow(shutterLabel_, SW_HIDE);
    ShowWindow(shutterEdit_, SW_HIDE);
    ShowWindow(samplesLabel_, SW_HIDE);
    ShowWindow(samplesEdit_, SW_HIDE);
    ShowWindow(temporalRestorationLabel_, SW_HIDE);
    ShowWindow(temporalRestorationCombo_, SW_HIDE);
    ShowWindow(restorationLabel_, SW_HIDE);
    ShowWindow(restorationCombo_, SW_HIDE);
    ShowWindow(outputSizeLabel_, SW_HIDE);
    ShowWindow(outputSizeCombo_, SW_HIDE);
    setFonts();
    for (const auto control : {sourceEdit_, sourceButton_, sourceFolderButton_, outputEdit_, outputButton_, rateCombo_, rateCustomEdit_, encoderCombo_, platformCombo_, motionBlurCheck_, shutterEdit_, samplesEdit_, modelButton_, runtimeButton_, advancedButton_, qualityDisclosureButton_, startButton_, pauseResumeButton_, cancelButton_, rejectionDetailsButton_, historyButton_, comparisonButton_, automaticDenoiseButton_, denoiseAdjustmentEdit_, denoiseAdjustmentResetButton_, deblurRestorationCombo_, automaticDetailButton_, detailAdjustmentEdit_, detailAdjustmentResetButton_, automaticCompressionButton_, compressionAdjustmentEdit_, compressionAdjustmentResetButton_, temporalRestorationCombo_, compressionRestorationCombo_, compressionStrengthCombo_, compressionTilingCheck_, restorationCombo_, outputSizeCombo_}) {
        if (!control) continue;
        const auto original = SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NativeApp::scrollChildProcedure));
        SetPropW(control, L"RifeLoveScrollChildProc", reinterpret_cast<HANDLE>(original));
        SetPropW(control, L"RifeLoveScrollChildOwner", reinterpret_cast<HANDLE>(this));
    }
    layoutControls();
    updateView();
}

void NativeApp::setFonts() {
    if (font_) DeleteObject(font_);
    const int height = -MulDiv(11, static_cast<int>(GetDpiForWindow(window_)), 72);
    font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    const std::array<HWND, 50> controls{sourceEdit_, sourceButton_, sourceFolderButton_, outputEdit_, outputButton_, rateCombo_, rateCustomEdit_, encoderCombo_, platformCombo_, motionBlurCheck_, shutterEdit_, samplesEdit_, modelButton_, runtimeButton_, advancedButton_, qualityDisclosureButton_, startButton_, pauseResumeButton_, cancelButton_, rejectionDetailsButton_, statusDetailsButton_, historyButton_, comparisonButton_, automaticDenoiseButton_, denoiseAdjustmentEdit_, denoiseAdjustmentResetButton_, deblurRestorationCombo_, automaticDetailButton_, detailAdjustmentEdit_, detailAdjustmentResetButton_, automaticCompressionButton_, compressionAdjustmentEdit_, compressionAdjustmentResetButton_, temporalRestorationCombo_, compressionRestorationCombo_, compressionStrengthCombo_, compressionTilingCheck_, restorationCombo_, outputSizeCombo_,
        sourceLabel_, outputLabel_, rateLabel_, encoderLabel_, modelLabel_, platformLabel_, shutterLabel_, samplesLabel_, temporalRestorationLabel_, restorationLabel_, outputSizeLabel_};
    for (const auto control : controls) if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (const auto combo : {rateCombo_, encoderCombo_, platformCombo_, temporalRestorationCombo_, deblurRestorationCombo_, compressionRestorationCombo_, compressionStrengthCombo_, restorationCombo_, outputSizeCombo_}) {
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scaled(32, dpiScale()));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, scaled(32, dpiScale()));
    }
}

void NativeApp::styleCombo(HWND combo) {
    if (!combo) return;
    const auto original = SetWindowLongPtrW(combo, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NativeApp::comboProcedure));
    SetPropW(combo, L"RifeLoveComboProc", reinterpret_cast<HANDLE>(original));
    SetPropW(combo, L"RifeLoveComboOwner", reinterpret_cast<HANDLE>(this));
}

void NativeApp::layoutControls() {
    if (!window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const float s = dpiScale();
    const auto layout = NativeLayout::from(static_cast<float>(client.right) / s,
        static_cast<float>(client.bottom) / s, state_.scrollOffset, qualityExpanded_);
    state_.scrollOffset = layout.scroll;
    const float left = layout.left + 20;
    const float leftWidth = layout.split - layout.left - 40;
    const float right = layout.right;
    const float second = layout.exportContentLeft();
    const float rightWidth = layout.exportContentWidth();
    const auto move = [&](HWND control, float x, float y, float width, float height, bool visible = true, bool fixed = false) {
        if (!control) return;
        SetPropW(control, L"RifeLoveFocusEligible", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(visible ? 1 : 0)));
        SetPropW(control, L"RifeLoveScrollY", reinterpret_cast<HANDLE>(fixed ? 0 : static_cast<INT_PTR>(y) + 1));
        SetPropW(control, L"RifeLoveScrollHeight", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(height)));
        if (!fixed) y -= layout.scroll;
        const bool inViewport = fixed || (y >= layout.contentTop && y + height <= layout.contentBottom);
        MoveWindow(control, scaled(static_cast<int>(x), s), scaled(static_cast<int>(y), s),
            scaled(static_cast<int>(width), s), scaled(static_cast<int>(height), s), TRUE);
        ShowWindow(control, visible && inViewport ? SW_SHOW : SW_HIDE);
    };
    const auto moveCombo = [&](HWND control, float x, float y, float width, float height, bool visible = true) {
        if (!control) return;
        const auto count = static_cast<int>(SendMessageW(control, CB_GETCOUNT, 0, 0));
        // The combo HWND is only the closed field. Its list is a separate
        // popup, so sizing the HWND to include the list makes it cover the
        // controls below it and breaks hit testing.
        move(control, x, y, width, height, visible);
        resizeComboPopup(control);
        // Keep the closed field aligned with its column, but let the popup
        // show complete model/preset names in narrow or high-DPI windows.
        int popupWidth = scaled(static_cast<int>(width), s);
        if (HDC dc = GetDC(control)) {
            {
                const ScopedDcState saved(dc);
                if (font_) SelectObject(dc, font_);
                for (int index = 0; index < count; ++index) {
                    const LRESULT length = SendMessageW(control, CB_GETLBTEXTLEN, index, 0);
                    if (length <= 0) continue;
                    std::wstring label(static_cast<std::size_t>(length) + 1, L'\0');
                    SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()));
                    SIZE extent{};
                    if (GetTextExtentPoint32W(dc, label.data(), static_cast<int>(length), &extent))
                        popupWidth = std::max(popupWidth, static_cast<int>(extent.cx) + scaled(48, s));
                }
            }
            ReleaseDC(control, dc);
        }
        const RECT work = monitorWorkArea(MonitorFromWindow(control, MONITOR_DEFAULTTONEAREST));
        const int available = std::max(1, static_cast<int>(work.right - work.left) - scaled(16, s));
        SendMessageW(control, CB_SETDROPPEDWIDTH, std::min(popupWidth, available), 0);
    };
    // Labels are painted together with the card, so all text shares the same
    // type scale and none inherits a mismatching STATIC background.
    for (const auto label : {sourceLabel_, outputLabel_, rateLabel_, encoderLabel_, modelLabel_,
        platformLabel_, shutterLabel_, samplesLabel_, temporalRestorationLabel_, restorationLabel_, outputSizeLabel_}) ShowWindow(label, SW_HIDE);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    SetWindowTextW(sourceFolderButton_, layout.compactImport ? L"文件夹" : L"导入文件夹");
    move(sourceFolderButton_, layout.compactImport ? left + 114 : layout.split - 244,
        layout.sourceActionsTop, layout.compactImport ? std::min(110.0F, leftWidth - 114) : 110, 34);
#else
    ShowWindow(sourceFolderButton_, SW_HIDE);
#endif
    move(sourceButton_, layout.compactImport ? left : layout.split - 126, layout.sourceActionsTop, 106, 34);
    move(sourceEdit_, left + 10, layout.sourceInputTop, leftWidth - 20, 42);
    const float qualityY = NativeLayout::qualityDisclosureHeight;
    move(motionBlurCheck_, left, 342, leftWidth, 34);
    move(qualityDisclosureButton_, left, 384, leftWidth, 34);
    move(automaticDenoiseButton_, layout.split - 130, 300 + qualityY, 110, 30, qualityExpanded_);
    // Each field leaves its painted label unobstructed. Automatic adjustment
    // owns a separate row rather than squeezing a signed value into a model
    // selector or changing the unrelated spatial/compression controls.
    moveCombo(temporalRestorationCombo_, left, 354 + qualityY, leftWidth, 36, qualityExpanded_);
    move(denoiseAdjustmentEdit_, left + 10, 420 + qualityY, leftWidth - 110, 30, qualityExpanded_);
    move(denoiseAdjustmentResetButton_, layout.split - 100, 420 + qualityY, 80, 30, qualityExpanded_);
    const float qualityOffset = NativeLayout::denoiseAdjustmentHeight;
    moveCombo(deblurRestorationCombo_, left, 420 + qualityOffset + qualityY, leftWidth, 36, qualityExpanded_);
    move(automaticDetailButton_, layout.split - 140, 459 + qualityOffset + qualityY, 120, 30, qualityExpanded_);
    move(detailAdjustmentEdit_, left + 10, 524 + qualityOffset + qualityY, leftWidth - 110, 30, qualityExpanded_);
    move(detailAdjustmentResetButton_, layout.split - 100, 524 + qualityOffset + qualityY, 80, 30, qualityExpanded_);
    const float outputImageOffset = qualityOffset + NativeLayout::deblurAdjustmentHeight;
    moveCombo(restorationCombo_, left, 420 + outputImageOffset + qualityY, leftWidth, 36, qualityExpanded_);
    moveCombo(outputSizeCombo_, left, 486 + outputImageOffset + qualityY, leftWidth, 36, qualityExpanded_);
    moveCombo(compressionRestorationCombo_, left, 552 + outputImageOffset + qualityY, (leftWidth - 8) * 0.68F, 36, qualityExpanded_);
    moveCombo(compressionStrengthCombo_, left + (leftWidth - 8) * 0.68F + 8, 552 + outputImageOffset + qualityY, (leftWidth - 8) * 0.32F, 36, qualityExpanded_);
    move(compressionTilingCheck_, left, 590 + outputImageOffset + qualityY, leftWidth, 30, qualityExpanded_);
    move(automaticCompressionButton_, layout.split - 140, 630 + outputImageOffset + qualityY, 120, 30, qualityExpanded_);
    move(compressionAdjustmentEdit_, left + 10, 693 + outputImageOffset + qualityY, leftWidth - 110, 30, qualityExpanded_);
    move(compressionAdjustmentResetButton_, layout.split - 100, 693 + outputImageOffset + qualityY, 80, 30, qualityExpanded_);
    const float finalQualityOffset = NativeLayout::qualityControlsHeight + qualityY;
    move(advancedButton_, layout.split - 110, 684 + finalQualityOffset, 90, 32, qualityExpanded_);
    const bool blurEnabled = SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    move(shutterEdit_, left + 68, 732 + finalQualityOffset, 64, 30, qualityExpanded_ && advancedExpanded_);
    move(samplesEdit_, left + (leftWidth < 280 ? 194 : 218), 732 + finalQualityOffset, leftWidth < 280 ? 50 : 54, 30, qualityExpanded_ && advancedExpanded_);
    EnableWindow(shutterEdit_, blurEnabled);
    EnableWindow(samplesEdit_, blurEnabled);
    const float exportY = layout.exportOffset;
    moveCombo(platformCombo_, second, 162 + exportY, rightWidth, 36);
    const bool customRate = SendMessageW(rateCombo_, CB_GETCURSEL, 0, 0) == static_cast<int>(kTargetRates.size());
    moveCombo(rateCombo_, second, 240 + exportY, customRate ? rightWidth * 0.55F : rightWidth, 38);
    move(rateCustomEdit_, second + rightWidth * 0.57F + 8, 246 + exportY, rightWidth * 0.43F - 16, 26, customRate);
    moveCombo(encoderCombo_, second, 318 + exportY, rightWidth, 36);
    move(outputEdit_, second + 10, 394 + exportY, rightWidth - 20, 42);
    move(outputButton_, layout.outputBrowseLeft(), 460 + exportY, layout.outputBrowseWidth(), 34);
    SetWindowTextW(modelButton_, layout.singleColumn ? L"模型" : L"本地模型中心");
    SetWindowTextW(runtimeButton_, layout.singleColumn ? L"运行" : L"选择运行库");
    move(modelButton_, layout.singleColumn ? 8 : 18, layout.resourceButtonsTop, layout.side - (layout.singleColumn ? 16 : 36), 32, true, true);
    move(runtimeButton_, layout.singleColumn ? 8 : 18, layout.resourceButtonsTop + 40, layout.side - (layout.singleColumn ? 16 : 36), 32, true, true);
    move(comparisonButton_, right - 250, 28, 112, 34, true, true);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    SetWindowTextW(historyButton_, layout.singleColumn ? L"记录" : L"任务历史");
    move(historyButton_, layout.singleColumn ? 8 : 12, 188, layout.side - (layout.singleColumn ? 16 : 24), 42, taskQueue_ != nullptr, true);
#else
    ShowWindow(historyButton_, SW_HIDE);
#endif
    const float actionTop = layout.dock + (layout.singleColumn ? 78 : 44);
    move(startButton_, right - 158, actionTop, 158, 44, true, true);
    move(pauseResumeButton_, right - 330, actionTop, 76, 44, true, true);
    move(cancelButton_, right - 246, actionTop, 76, 44, true, true);
    move(statusDetailsButton_, left + 80, layout.dock + (layout.singleColumn ? 4 : 12), 88, 26, true, true);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    move(rejectionDetailsButton_, left + 176, layout.dock + (layout.singleColumn ? 4 : 12), 88, 26, !queueRejections_.empty(), true);
#else
    ShowWindow(rejectionDetailsButton_, SW_HIDE);
#endif
    const int range = std::max(0, static_cast<int>(layout.contentEnd - layout.contentBottom));
    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(SCROLLINFO); scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0; scroll.nMax = range; scroll.nPage = 1; scroll.nPos = static_cast<int>(layout.scroll);
    SetScrollInfo(window_, SB_VERT, &scroll, TRUE);
    ShowScrollBar(window_, SB_VERT, range > 0);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    updateRestorationControls();
#endif
}

void NativeApp::resizeComboPopup(HWND combo) const {
    if (!combo) return;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    const int rowHeight = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
    if (count <= 0 || rowHeight <= 0) return;

    const int visibleItems = std::clamp(count, 1, 8);
    SendMessageW(combo, CB_SETMINVISIBLE, static_cast<WPARAM>(visibleItems), 0);

    COMBOBOXINFO comboInfo{};
    comboInfo.cbSize = sizeof(comboInfo);
    if (!GetComboBoxInfo(combo, &comboInfo) || !comboInfo.hwndList) return;
    RECT listRect{};
    RECT fieldRect{};
    if (!GetWindowRect(comboInfo.hwndList, &listRect) || !GetWindowRect(combo, &fieldRect)) return;

    const float scale = dpiScale();
    const RECT work = monitorWorkArea(MonitorFromWindow(combo, MONITOR_DEFAULTTONEAREST));
    const int workHeight = std::max(1L, work.bottom - work.top);
    int popupHeight = std::min(workHeight, rowHeight * visibleItems + scaled(8, scale));
    const int below = std::max(0L, work.bottom - fieldRect.bottom);
    const int above = std::max(0L, fieldRect.top - work.top);
    int top = fieldRect.bottom;
    if (below >= popupHeight) {
        // Prefer the natural position immediately below the closed field.
        top = fieldRect.bottom;
    } else if (above >= popupHeight) {
        // If the lower edge is tight, keep the popup immediately above it.
        top = fieldRect.top - popupHeight;
    } else if (below >= above) {
        // A very small work area cannot fit all rows. Keep the popup attached
        // below and let the native list scroll through the remaining items.
        popupHeight = std::max(1, below);
        top = fieldRect.bottom;
    } else {
        popupHeight = std::max(1, above);
        top = fieldRect.top - popupHeight;
    }
    const int workTop = static_cast<int>(work.top);
    const int workBottom = static_cast<int>(work.bottom);
    top = std::clamp(top, workTop, std::max(workTop, workBottom - popupHeight));
    const int width = listRect.right - listRect.left;
    if (width <= 0) return;
    SetWindowPos(comboInfo.hwndList, nullptr, listRect.left, top, width, popupHeight,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

void NativeApp::scrollSettings(int delta) {
    state_.scrollOffset = std::max(0.0F, state_.scrollOffset + static_cast<float>(delta));
    layoutControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void NativeApp::ensureControlVisible(HWND control) {
    if (!window_ || !control) return;
    const auto encoded = reinterpret_cast<INT_PTR>(GetPropW(control, L"RifeLoveScrollY"));
    if (encoded == 0) return;
    const float y = static_cast<float>(encoded - 1);
    const float height = static_cast<float>(reinterpret_cast<INT_PTR>(GetPropW(control, L"RifeLoveScrollHeight")));
    RECT client{}; GetClientRect(window_, &client);
    const auto layout = NativeLayout::from(static_cast<float>(client.right) / dpiScale(), static_cast<float>(client.bottom) / dpiScale(), state_.scrollOffset, qualityExpanded_);
    float desired = layout.scroll;
    if (y < layout.scroll + layout.contentTop + 8) desired = y - layout.contentTop - 8;
    else if (y + height > layout.scroll + layout.contentBottom - 8) desired = y + height - layout.contentBottom + 8;
    desired = NativeLayout::from(layout.width, layout.height, desired, qualityExpanded_).scroll;
    if (desired != state_.scrollOffset) { state_.scrollOffset = desired; layoutControls(); InvalidateRect(window_, nullptr, FALSE); }
}

bool NativeApp::focusNextControl(bool reverse) {
    const std::array<HWND, 39> order{sourceButton_, sourceFolderButton_, sourceEdit_, motionBlurCheck_, qualityDisclosureButton_, automaticDenoiseButton_, temporalRestorationCombo_, denoiseAdjustmentEdit_, denoiseAdjustmentResetButton_, deblurRestorationCombo_, automaticDetailButton_, detailAdjustmentEdit_, detailAdjustmentResetButton_, restorationCombo_, outputSizeCombo_, compressionRestorationCombo_, compressionStrengthCombo_, compressionTilingCheck_, automaticCompressionButton_, compressionAdjustmentEdit_, compressionAdjustmentResetButton_,
        advancedButton_, shutterEdit_, samplesEdit_, platformCombo_, rateCombo_, rateCustomEdit_, encoderCombo_, outputEdit_, outputButton_,
        modelButton_, runtimeButton_, comparisonButton_, historyButton_, statusDetailsButton_, pauseResumeButton_, cancelButton_, startButton_, rejectionDetailsButton_};
    const auto found = std::find(order.begin(), order.end(), GetFocus());
    const int initial = found == order.end() ? (reverse ? 0 : -1) : static_cast<int>(found - order.begin());
    for (int distance = 1; distance <= static_cast<int>(order.size()); ++distance) {
        const int index = (initial + (reverse ? -distance : distance) + static_cast<int>(order.size()) * 2) % static_cast<int>(order.size());
        const HWND control = order[static_cast<std::size_t>(index)];
        if (!control || !GetPropW(control, L"RifeLoveFocusEligible") || !IsWindowEnabled(control)) continue;
        ensureControlVisible(control);
        if (!IsWindowVisible(control)) continue;
        SetFocus(control);
        return GetFocus() == control;
    }
    return false;
}

LRESULT CALLBACK NativeApp::scrollChildProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"RifeLoveScrollChildProc"));
    auto* app = reinterpret_cast<NativeApp*>(GetPropW(window, L"RifeLoveScrollChildOwner"));
    if (app && message == WM_MOUSEWHEEL) {
        wchar_t name[32]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
        const bool openList = _wcsicmp(name, L"COMBOBOX") == 0 && SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0);
        if (!openList) return SendMessageW(app->window_, message, wParam, lParam);
    }
    if (app && message == WM_SETFOCUS) app->ensureControlVisible(window);
    if (message == WM_NCDESTROY) {
        for (const auto* property : {L"RifeLoveScrollChildProc", L"RifeLoveScrollChildOwner", L"RifeLoveFocusEligible", L"RifeLoveScrollY", L"RifeLoveScrollHeight"}) RemovePropW(window, property);
    }
    return original ? CallWindowProcW(original, window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

void NativeApp::updateView() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (rejectionDetailsButton_) ShowWindow(rejectionDetailsButton_, queueRejections_.empty() ? SW_HIDE : SW_SHOW);
    // Reset unavailable-model opt-ins before composing summaries or captured
    // settings so a removed model never leaves one stale automatic frame.
    updateRestorationControls();
#endif
    state_.sourcePath = pathText(currentSource());
    state_.outputPath = pathText(currentOutput());
    state_.mediaSummary = mediaSummary_;
    state_.modelSummary = modelSummary_;
    state_.runtimeSummary = runtimeSummary_;
    state_.deviceSummary = deviceSummary_;
    state_.platformSummary = platformSummary_;
    const auto rateSelection = static_cast<int>(SendMessageW(rateCombo_, CB_GETCURSEL, 0, 0));
    const auto rateText = rateSelection >= 0 && rateSelection < static_cast<int>(kTargetRates.size())
        ? std::to_wstring(kTargetRates[static_cast<std::size_t>(rateSelection)]) : controlText(rateCustomEdit_);
    const auto extension = currentOutput().extension().wstring();
    state_.platformSummary += L"\n当前：" + rateText + L" FPS · " +
        (extension.empty() ? (lastPlatformSelection_ > 0 ? std::wstring(L".mp4") : std::wstring(L".mkv")) : extension) +
        L" · " + controlText(encoderCombo_);
    try {
        PipelineSettings settings;
        applyExportControls(settings);
        if (lastPlatformSelection_ > 0) {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            if (hasBatchSelection()) state_.platformSummary += L"\n码率与 GOP 将按每个视频计算";
            else
#endif
            state_.platformSummary += L"\n" + std::to_wstring(settings.encoding.bitRate / 1000) +
                L" kbps · GOP " + std::to_wstring(settings.encoding.gopSize) + L" 帧";
        }
    } catch (const std::exception&) {
        state_.platformSummary += L"\n导出参数无效，请检查帧率与曝光";
    }
    const bool blurEnabled = SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state_.motionBlurSummary = blurEnabled
        ? L"自然曝光：" + controlText(shutterEdit_) + L"° · " + controlText(samplesEdit_) + L" 次采样；参数可修改"
        : L"自然曝光：关闭；可手动开启并修改参数";
    state_.restorationSummary = L"降噪：关闭 · 去模糊：关闭 · 超分：关闭";
    state_.denoiseModeSummary = L"可选降噪：关闭 · 默认项见模型中心";
    state_.denoiseAdjustmentSummary = L"请选择降噪模型并启用自动模式；0% 跟随自动。";
    state_.detailSummary = L"实验性去模糊：关闭";
    state_.detailAdjustmentSummary = L"默认关闭；仅验证 SDR8。可能过锐或伪影，并增加推理与显存开销。";
    state_.compressionSummary = L"默认关闭 · CUDA / BT.709 SDR8 · 仅验证至 384 × 256";
    state_.compressionAdjustmentSummary = L"自动模式关闭；手动强度保持独立。";
    state_.outputSizeSummary = L"原始输出尺寸";
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    const int temporalIndex = temporalRestorationCombo_ ? static_cast<int>(SendMessageW(temporalRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int deblurIndex = deblurRestorationCombo_ ? static_cast<int>(SendMessageW(deblurRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const int restorationIndex = restorationCombo_ ? static_cast<int>(SendMessageW(restorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const bool hasTemporal = temporalIndex > 0 && temporalIndex <= static_cast<int>(temporalRestorationModels_.size());
    const bool hasDeblur = deblurIndex > 0 && deblurIndex <= static_cast<int>(deblurRestorationModels_.size());
    const bool hasSpatial = restorationIndex > 0 && restorationIndex <= static_cast<int>(spatialRestorationModels_.size());
    if (hasTemporal || hasDeblur || hasSpatial) {
        state_.restorationSummary.clear();
        state_.restorationSummary += L"降噪：";
        state_.restorationSummary += hasTemporal
            ? (automaticTemporalDenoise_ ? L"FastDVDnet 自动高斯*" : L"FastDVDnet 固定 σ25")
            : L"关闭";
        state_.restorationSummary += L" · 去模糊：";
        state_.restorationSummary += hasDeblur ? (automaticDetailRestoration_ ? L"NAFNet 自动*" : L"NAFNet 固定") : L"关闭";
        state_.restorationSummary += L" · 超分：";
        state_.restorationSummary += hasSpatial ? L"×2" : L"关闭";
    }
    if (hasTemporal) state_.denoiseModeSummary = automaticTemporalDenoise_
        ? L"实验自动高斯降噪 · 软件解码 / 恢复重放"
        : L"固定 σ25 · 自动估计关闭（仍降噪）";
    if (hasTemporal) {
        state_.denoiseAdjustmentSummary = automaticTemporalDenoise_
            ? L"−100%…+100% · 0 跟随自动；负值更轻，正值更强。"
            : L"自动模式关闭：固定 σ25；相对偏移不生效。";
        if (automaticTemporalDenoise_) {
            try { (void)nativeRelativeAdjustment(controlText(denoiseAdjustmentEdit_)); }
            catch (const std::exception&) { state_.denoiseAdjustmentSummary = L"偏移无效：请输入 −100 到 +100 的数值；0 跟随自动。"; }
        }
    }
    if (hasDeblur) {
        state_.detailSummary = automaticDetailRestoration_ ? L"实验性自动细节：NAFNet 去模糊" : L"实验性固定去模糊：NAFNet";
        state_.detailAdjustmentSummary = automaticDetailRestoration_
            ? L"−100%…+100%；0 跟随自动。仅 SDR8；增加推理、显存和重放耗时。"
            : L"固定去模糊已启用；自动偏移不生效。仅 SDR8，可能过锐或伪影。";
        if (automaticDetailRestoration_) {
            try { (void)nativeRelativeAdjustment(controlText(detailAdjustmentEdit_)); }
            catch (const std::exception&) { state_.detailAdjustmentSummary = L"自动偏移无效：请输入 −100 到 +100。"; }
        }
    }
    if (automaticDetailRestoration_ && blurEnabled) {
        try {
            if (parsePositiveInteger(controlText(shutterEdit_), "shutter angle") > 0 &&
                parsePositiveInteger(controlText(samplesEdit_), "sample count") > 1)
                state_.detailAdjustmentSummary = L"冲突：自动细节与自然动态曝光合成不能同时开启。";
        } catch (const std::exception&) { /* Exposure field validation explains invalid values. */ }
    }
    const int compressionStrengthIndex = compressionStrengthCombo_ ? static_cast<int>(SendMessageW(compressionStrengthCombo_, CB_GETCURSEL, 0, 0)) : 0;
    if (compressionRestorationModels_.empty())
        state_.compressionSummary = L"关闭 · 请在本地模型中心登记已验证 STDF；仅支持 CUDA / SDR8 / 384 × 256";
    else if (compressionStrengthIndex > 0 && compressionStrengthIndex <= 3) {
        const bool tiled = compressionTilingCheck_ && SendMessageW(compressionTilingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state_.compressionSummary = L"STDF " + std::to_wstring(static_cast<int>(nativeCompressionStrength(compressionStrengthIndex) * 100)) +
            (tiled ? L"% · 高清分块评估 / 较慢 · CUDA / BT.709 SDR8" : L"% · 整帧评估 · CUDA / BT.709 SDR8 / ≤384 × 256");
        state_.restorationSummary += L" · 压缩修复：STDF";
        state_.compressionAdjustmentSummary = automaticCompressionRestoration_
            ? L"−100%…+100% · 0 跟随自动；低置信度帧自动跳过。"
            : L"自动模式关闭：使用上方手动强度；相对偏移不生效。";
        if (automaticCompressionRestoration_) {
            try { (void)nativeRelativeAdjustment(controlText(compressionAdjustmentEdit_)); }
            catch (const std::exception&) { state_.compressionAdjustmentSummary = L"偏移无效：请输入 −100 到 +100 的数值；0 跟随自动。"; }
        }
    }
    const int outputSizeIndex = outputSizeCombo_ ? static_cast<int>(SendMessageW(outputSizeCombo_, CB_GETCURSEL, 0, 0)) : 0;
    if (outputSizeIndex > 0) state_.outputSizeSummary = outputTargetLabel(outputSizeIndex);
#endif
    state_.advancedExpanded = advancedExpanded_;
    state_.qualityExpanded = qualityExpanded_;
    state_.automaticTemporalDenoise = automaticTemporalDenoise_;
    state_.automaticDetailRestoration = automaticDetailRestoration_;
    state_.automaticCompressionRestoration = automaticCompressionRestoration_;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    state_.sourceReady = sourceInspected_;
    if (modelLabel_) SetWindowTextW(modelLabel_, state_.modelReady && state_.runtimeReady ? L"模型和运行库 · 已就绪" : L"模型和运行库 · 待检查");
#endif
    if (window_) {
        InvalidateRect(window_, nullptr, FALSE);
        updateButtonState();
    }
}

void NativeApp::updateButtonState() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    const bool serviceBusy = (processingService_ && processingService_->busy()) || queueSessionActive_.load();
    const auto readiness = evaluateStartReadiness();
    const bool ready = readiness.ready;
    if (!state_.processing && state_.stage != L"failed" && state_.stage != L"completed" && state_.stage != L"cancelled")
        state_.status = readiness.status;
#else
    const bool serviceBusy = false;
    const bool ready = false;
#endif
    if (startButton_) EnableWindow(startButton_, ready);
    if (cancelButton_) EnableWindow(cancelButton_, serviceBusy);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (pauseResumeButton_) {
        const bool canTogglePause = taskQueue_ && taskQueue_->busy() && !queuePauseTransitionPending_;
        EnableWindow(pauseResumeButton_, canTogglePause);
        SetWindowTextW(pauseResumeButton_, queuePaused_ ? L"继续" : L"暂停");
    }
#else
    if (pauseResumeButton_) EnableWindow(pauseResumeButton_, FALSE);
#endif
    for (const auto control : {sourceEdit_, sourceButton_, sourceFolderButton_, outputEdit_, outputButton_, rateCombo_,
        rateCustomEdit_, encoderCombo_, platformCombo_, modelButton_, runtimeButton_, motionBlurCheck_,
        temporalRestorationCombo_, denoiseAdjustmentEdit_, denoiseAdjustmentResetButton_, deblurRestorationCombo_, automaticDetailButton_, detailAdjustmentEdit_, detailAdjustmentResetButton_, compressionRestorationCombo_, compressionStrengthCombo_, compressionTilingCheck_,
        automaticCompressionButton_, compressionAdjustmentEdit_, compressionAdjustmentResetButton_, restorationCombo_, outputSizeCombo_}) if (control) EnableWindow(control, !serviceBusy);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    EnableWindow(automaticDenoiseButton_, !serviceBusy);
#endif
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (!serviceBusy) updateRestorationControls();
    updateModelDetails();
#else
    if (modelButton_) EnableWindow(modelButton_, FALSE);
    EnableWindow(denoiseAdjustmentEdit_, FALSE);
    EnableWindow(denoiseAdjustmentResetButton_, FALSE);
    EnableWindow(deblurRestorationCombo_, FALSE);
    EnableWindow(automaticDetailButton_, FALSE);
    EnableWindow(detailAdjustmentEdit_, FALSE);
    EnableWindow(detailAdjustmentResetButton_, FALSE);
    EnableWindow(automaticCompressionButton_, FALSE);
    EnableWindow(compressionAdjustmentEdit_, FALSE);
    EnableWindow(compressionAdjustmentResetButton_, FALSE);
#endif
}

void NativeApp::showStatusDetails() {
    std::wostringstream report;
    report << L"当前状态快照（重新打开可刷新）\r\n\r\n"
        << L"完整状态\r\n" << state_.status << L"\r\n\r\n"
        << L"处理阶段\r\n" << (state_.stage.empty() ? L"尚未开始" : state_.stage) << L"\r\n"
        << L"已生成 " << state_.outputFrames << L" 帧 · " << std::fixed << std::setprecision(1)
        << state_.processingFps << L" 处理 FPS\r\n\r\n"
        << L"源视频\r\n" << currentSource().wstring() << L"\r\n" << state_.mediaSummary << L"\r\n\r\n"
        << L"输出文件\r\n" << currentOutput().wstring() << L"\r\n\r\n"
        << L"导出设置\r\n" << state_.platformSummary << L"\r\n\r\n"
        << L"显卡\r\n" << state_.deviceSummary << L"\r\n\r\n"
        << L"模型\r\n" << state_.modelSummary << L"\r\n\r\n"
        << L"推理运行库\r\n" << state_.runtimeSummary << L"\r\n\r\n"
        << L"画质设置\r\n" << state_.restorationSummary << L"\r\n" << state_.detailSummary << L"\r\n"
        << state_.detailAdjustmentSummary << L"\r\n"
        << state_.compressionSummary << L"\r\n" << state_.motionBlurSummary;
    if (!statusWindow_.show(window_, report.str()))
        MessageBoxW(window_, L"无法创建任务详情窗口，请稍后重试。", L"RifeLove", MB_OK | MB_ICONERROR);
}

void NativeApp::resizeForAdvancedSettings() {
    // The compact layout keeps the action row reachable in the default
    // 768-pixel client area, so expanding settings does not move the window
    // or hide the primary action behind the fold.
    if (window_) layoutControls();
}

void NativeApp::toggleAdvancedSettings() {
    advancedExpanded_ = !advancedExpanded_;
    if (advancedButton_) SetWindowTextW(advancedButton_, advancedExpanded_ ? L"收起  ˄" : L"参数  ˅");
    resizeForAdvancedSettings();
    layoutControls();
    updateView();
}

void NativeApp::setSourcePath(const std::filesystem::path& source) {
    if (source.empty()) return;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    queuedInputs_.clear();
    queuedInputs_.push_back(source);
#endif
    SetWindowTextW(sourceEdit_, source.c_str());
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    std::error_code sourceError;
    const bool directory = std::filesystem::is_directory(source, sourceError);
    if (directory) {
        setOutputPath(source);
        sourceInspected_ = false;
        sourceColorSupported_ = false;
        encoderAvailable_ = false;
        encoderCapabilities_.clear();
        encoderCapabilitiesYuv420p_.clear();
        deviceAvailable_ = false;
        mediaSummary_.clear();
        state_.encoderSummary.clear();
        state_.status = L"已载入文件夹；点击开始后将批量检查其中的视频";
        updateView();
        return;
    }
#endif
    auto output = source;
    const int platformIndex = static_cast<int>(SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0));
    output.replace_filename(source.stem().wstring() + (platformIndex > 0 ? L"_RifeLove.mp4" : L"_RifeLove.mkv"));
    setOutputPath(output);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    sourceInspected_ = false;
    sourceColorSupported_ = false;
    encoderAvailable_ = false;
    encoderCapabilities_.clear();
    encoderCapabilitiesYuv420p_.clear();
    deviceAvailable_ = false;
    state_.encoderSummary.clear();
    mediaSummary_.clear();
    state_.status = L"正在检查视频和本机编码器…";
    inspectSource();
#else
    state_.status = L"当前构建未启用完整媒体服务";
#endif
    updateView();
}

void NativeApp::selectSource() {
    wchar_t fileName[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrFilter = L"视频文件\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.ts\0所有文件\0*.*\0\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    setSourcePath(std::filesystem::path(fileName));
}

void NativeApp::selectSourceFolder() {
    const auto directory = browseForFolder(L"选择包含视频的文件夹；开始后将批量加入本地队列");
    if (directory.empty()) return;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    setSourcePath(directory);
#else
    state_.status = L"当前构建未启用完整媒体服务";
    SetWindowTextW(sourceEdit_, directory.c_str());
    updateView();
#endif
}

void NativeApp::selectOutput() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (hasBatchSelection()) {
        const auto directory = browseForFolder(L"选择批量输出文件夹");
        if (!directory.empty()) setOutputPath(directory);
        updateView();
        return;
    }
#endif
    wchar_t fileName[32768]{};
    const auto existing = currentOutput().wstring();
    if (!existing.empty()) wcsncpy_s(fileName, existing.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFile = fileName;
    dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
    dialog.lpstrFilter = L"MKV 视频\0*.mkv\0MP4 视频\0*.mp4\0所有文件\0*.*\0\0";
    const bool platformCopy = SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0) > 0;
    dialog.nFilterIndex = platformCopy ? 2 : 1;
    dialog.lpstrDefExt = platformCopy ? L"mp4" : L"mkv";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) {
        setOutputPath(std::filesystem::path(fileName));
        std::error_code outputError;
        state_.status = std::filesystem::exists(std::filesystem::path(fileName), outputError)
            ? L"输出文件已存在，请选择新的输出位置"
            : L"输出位置已设置，可以继续检查条件";
    }
    updateView();
}

std::filesystem::path NativeApp::browseForFolder(const wchar_t* title) const {
    BROWSEINFOW info{};
    info.hwndOwner = window_;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE list = SHBrowseForFolderW(&info);
    if (!list) return {};
    wchar_t path[MAX_PATH]{};
    const bool success = SHGetPathFromIDListW(list, path) != FALSE;
    CoTaskMemFree(list);
    return success ? std::filesystem::path(path) : std::filesystem::path{};
}

void NativeApp::selectModel() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    showModelCenter();
#endif
}

void NativeApp::selectRuntime() {
    const auto directory = browseForFolder(L"选择包含 ONNX Runtime CUDA DLL 的本地目录");
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (!directory.empty() && refreshRuntime(directory)) {
        state_.status = L"推理运行库已检查";
        updateView();
    }
#else
    (void)directory;
#endif
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
LRESULT CALLBACK NativeApp::modelCenterWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->modelCenterWindow_ = window;
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_TIMER:
        if (wParam == kModelInstallTimer) { app->pollModelPackageInstall(); return 0; }
        break;
    case WM_CREATE: {
        app->modelCenterBackgroundBrush_ = CreateSolidBrush(RGB(12, 15, 21));
        app->modelCenterSurfaceBrush_ = CreateSolidBrush(RGB(19, 24, 32));
        const auto control = [&](const wchar_t* cls, const wchar_t* label, DWORD style, int id) {
            return CreateWindowExW((id == kModelList || id == kModelDetails) ? WS_EX_CLIENTEDGE : 0,
                cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app->instance_, nullptr);
        };
        app->modelCenterStatus_ = control(L"STATIC", L"", SS_LEFT, 1200);
        app->modelDownloadUrl_ = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kModelDownloadUrl);
        SendMessageW(app->modelDownloadUrl_, EM_SETLIMITTEXT, 32768, 0);
        SendMessageW(app->modelDownloadUrl_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"HTTPS ZIP 地址；多个镜像用分号分隔"));
        control(L"BUTTON", L"下载并安装", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelDownload);
        app->modelList_ = control(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kModelList);
        app->modelDetails_ = control(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, kModelDetails);
        app->modelListHeading_ = control(L"STATIC", L"已安装与可用模型", SS_LEFT | SS_NOPREFIX, kModelListHeading);
        app->modelDetailsHeading_ = control(L"STATIC", L"模型详情", SS_LEFT | SS_NOPREFIX, kModelDetailsHeading);
        SendMessageW(app->modelDetails_, EM_SETLIMITTEXT, 128 * 1024, 0);
        control(L"BUTTON", L"登记本地目录…", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelRegister);
        control(L"BUTTON", L"安装验证包…", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelInstall);
        control(L"BUTTON", L"取消安装", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelCancelInstall);
        control(L"BUTTON", L"设为该类默认", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelDefault);
        control(L"BUTTON", L"取消登记", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelUnregister);
        control(L"BUTTON", L"重新校验", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelRefresh);
        control(L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW, kModelClose);
        app->layoutModelCenter();
        app->updateModelCenter();
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        const auto brush = app->modelCenterBackgroundBrush_ ? app->modelCenterBackgroundBrush_ : GetSysColorBrush(COLOR_WINDOW);
        FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        const bool surface = message != WM_CTLCOLORSTATIC;
        const auto controlId = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        const bool heading = controlId == kModelListHeading || controlId == kModelDetailsHeading;
        const bool primaryText = surface || heading || controlId == 1200;
        SetTextColor(dc, primaryText ? RGB(225, 232, 244) : RGB(159, 178, 201));
        SetBkColor(dc, surface ? RGB(19, 24, 32) : RGB(12, 15, 21));
        const auto brush = surface ? app->modelCenterSurfaceBrush_ : app->modelCenterBackgroundBrush_;
        return reinterpret_cast<LRESULT>(brush ? brush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!item || item->CtlType != ODT_BUTTON) break;
        const bool primary = static_cast<int>(item->CtlID) == kModelDefault || static_cast<int>(item->CtlID) == kModelDownload;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;
        const COLORREF fill = disabled ? RGB(30, 36, 46) : primary
            ? (pressed ? RGB(118, 202, 170) : RGB(162, 231, 203))
            : (pressed ? RGB(37, 49, 63) : RGB(27, 34, 45));
        const COLORREF text = disabled ? RGB(104, 116, 133) : primary ? RGB(16, 36, 29) : RGB(225, 232, 244);
        const COLORREF border = disabled ? RGB(39, 46, 57) : primary ? RGB(162, 231, 203) : RGB(57, 69, 86);
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        const int edge = MulDiv(10, dpi ? dpi : 96, 96);
        const int savedDc = SaveDC(item->hDC);
        const int oldBackgroundMode = SetBkMode(item->hDC, TRANSPARENT);
        const COLORREF oldTextColor = SetTextColor(item->hDC, text);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, MulDiv(1, dpi ? dpi : 96, 96)), border);
        HBRUSH brush = CreateSolidBrush(fill);
        const auto oldPen = pen ? SelectObject(item->hDC, pen) : nullptr;
        const auto oldBrush = brush ? SelectObject(item->hDC, brush) : nullptr;
        RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, edge, edge);
        if (oldPen) SelectObject(item->hDC, oldPen);
        if (oldBrush) SelectObject(item->hDC, oldBrush);
        if (pen) DeleteObject(pen);
        if (brush) DeleteObject(brush);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
        const auto oldFont = font ? SelectObject(item->hDC, font) : nullptr;
        wchar_t label[256]{};
        GetWindowTextW(item->hwndItem, label, static_cast<int>(std::size(label)));
        RECT textRect = item->rcItem;
        DrawTextW(item->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (oldFont) SelectObject(item->hDC, oldFont);
        if (focused) {
            RECT focusRect = item->rcItem;
            InflateRect(&focusRect, -MulDiv(4, dpi ? dpi : 96, 96), -MulDiv(4, dpi ? dpi : 96, 96));
            DrawFocusRect(item->hDC, &focusRect);
        }
        if (savedDc) RestoreDC(item->hDC, savedDc);
        else {
            if (oldFont) SelectObject(item->hDC, oldFont);
            if (oldPen) SelectObject(item->hDC, oldPen);
            if (oldBrush) SelectObject(item->hDC, oldBrush);
            if (oldBackgroundMode) SetBkMode(item->hDC, oldBackgroundMode);
            if (oldTextColor != CLR_INVALID) SetTextColor(item->hDC, oldTextColor);
        }
        return TRUE;
    }
    case WM_SIZE:
        app->layoutModelCenter();
        return 0;
    case WM_DPICHANGED: {
        if (app->modelCenterFont_) { DeleteObject(app->modelCenterFont_); app->modelCenterFont_ = nullptr; }
        if (app->modelCenterHeadingFont_) { DeleteObject(app->modelCenterHeadingFont_); app->modelCenterHeadingFont_ = nullptr; }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            const auto work = monitorWorkArea(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
            const auto fitted = fitWindowToWorkArea(*suggested, work);
            SetWindowPos(window, nullptr, fitted.left, fitted.top, fitted.right - fitted.left, fitted.bottom - fitted.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        app->layoutModelCenter();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const auto work = monitorWorkArea(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        info->ptMinTrackSize.x = std::min<LONG>(MulDiv(480, dpi ? dpi : 96, 96), work.right - work.left);
        info->ptMinTrackSize.y = std::min<LONG>(MulDiv(480, dpi ? dpi : 96, 96), work.bottom - work.top);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kModelList && HIWORD(wParam) == LBN_SELCHANGE) {
            app->updateModelDetails();
            return 0;
        }
        if (HIWORD(wParam) != BN_CLICKED) break;
        const auto index = static_cast<std::size_t>(SendMessageW(app->modelList_, LB_GETCURSEL, 0, 0));
        if (id == kModelClose || id == IDCANCEL) {
            if (app->modelInstallThread_.joinable()) {
                app->closeModelCenterWhenInstallEnds_ = true;
                if (app->modelInstallActive_.load()) {
                    app->cancelModelPackageInstall();
                    app->modelCenterNotice_ = L"正在安全取消模型安装；现有模型仍可使用。";
                    app->updateModelDetails();
                } else app->pollModelPackageInstall();
            } else DestroyWindow(window);
            return 0;
        }
        if (id == kModelRegister && app->modelChangesAllowed()) {
            const auto directory = app->browseForFolder(L"登记本机模型目录（包含 model.json）；原文件保留不变");
            if (!directory.empty()) (void)app->registerLocalModel(directory);
        } else if (id == kModelDownload && app->modelChangesAllowed() && !app->modelInstallActive_.load()) {
            const auto input = app->controlText(app->modelDownloadUrl_);
            std::vector<std::wstring> mirrorUrls;
            std::size_t begin = 0;
            while (begin < input.size()) {
                const auto end = input.find_first_of(L";\r\n", begin);
                auto mirror = input.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
                const auto first = mirror.find_first_not_of(L" \t");
                if (first != std::wstring::npos) {
                    const auto last = mirror.find_last_not_of(L" \t");
                    mirror = mirror.substr(first, last - first + 1);
                    mirrorUrls.push_back(std::move(mirror));
                }
                if (end == std::wstring::npos) break;
                begin = end + 1;
            }
            if (mirrorUrls.empty() || mirrorUrls.size() > 5) {
                app->modelCenterNotice_ = L"请填入 1–5 个 HTTPS ZIP 镜像链接，以分号分隔。";
                app->updateModelDetails();
            } else app->startModelPackageInstall({}, std::move(mirrorUrls));
        } else if (id == kModelInstall && app->modelChangesAllowed() && !app->modelInstallActive_.load()) {
            std::array<wchar_t, 32768> selected{};
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = window;
            dialog.lpstrFilter = L"RifeLove 模型包 (*.zip)\0*.zip\0ZIP 文件 (*.zip)\0*.zip\0\0";
            dialog.lpstrFile = selected.data();
            dialog.nMaxFile = static_cast<DWORD>(selected.size());
            dialog.lpstrTitle = L"选择模型 ZIP 包（需与 .distribution.json 清单放在同一目录）";
            dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&dialog)) {
                auto archive = std::filesystem::path(selected.data());
                auto manifest = archive;
                manifest.replace_extension(L".distribution.json");
                const DWORD attributes = GetFileAttributesW(manifest.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                    MessageBoxW(window, L"未找到与 ZIP 同名的 .distribution.json 清单。请从同一模型下载渠道同时取得两个文件。",
                        L"缺少模型清单", MB_OK | MB_ICONWARNING);
                } else app->startModelPackageInstall(archive);
            }
        } else if (id == kModelCancelInstall) {
            app->cancelModelPackageInstall();
        } else if (id == kModelDefault) (void)app->setDefaultLocalModel(index);
        else if (id == kModelUnregister) (void)app->unregisterLocalModel(index);
        else if (id == kModelRefresh && app->modelChangesAllowed()) {
            app->modelCenterNotice_ = L"已重新读取并校验本地模型；缺失或校验失败的模型不能设为默认。";
            app->refreshLocalModels();
        }
        return 0;
    }
    case WM_CLOSE:
        if (app->modelInstallThread_.joinable()) {
            app->closeModelCenterWhenInstallEnds_ = true;
            if (app->modelInstallActive_.load()) {
                app->cancelModelPackageInstall();
                app->modelCenterNotice_ = L"正在安全取消模型安装；现有模型仍可使用。";
                app->updateModelDetails();
            } else app->pollModelPackageInstall();
        } else DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        KillTimer(window, kModelInstallTimer);
        app->modelCenterWindow_ = nullptr;
        app->modelList_ = nullptr;
        app->modelDetails_ = nullptr;
        app->modelCenterStatus_ = nullptr;
        app->modelDownloadUrl_ = nullptr;
        app->modelListHeading_ = nullptr;
        app->modelDetailsHeading_ = nullptr;
        if (app->modelCenterFont_) { DeleteObject(app->modelCenterFont_); app->modelCenterFont_ = nullptr; }
        if (app->modelCenterHeadingFont_) { DeleteObject(app->modelCenterHeadingFont_); app->modelCenterHeadingFont_ = nullptr; }
        if (app->modelCenterBackgroundBrush_) { DeleteObject(app->modelCenterBackgroundBrush_); app->modelCenterBackgroundBrush_ = nullptr; }
        if (app->modelCenterSurfaceBrush_) { DeleteObject(app->modelCenterSurfaceBrush_); app->modelCenterSurfaceBrush_ = nullptr; }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void NativeApp::layoutModelCenter() {
    if (!modelCenterWindow_) return;
    const int dpi = static_cast<int>(GetDpiForWindow(modelCenterWindow_));
    const auto px = [&](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
    if (!modelCenterFont_) modelCenterFont_ = CreateFontW(-MulDiv(10, dpi ? dpi : 96, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!modelCenterHeadingFont_) modelCenterHeadingFont_ = CreateFontW(-MulDiv(11, dpi ? dpi : 96, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    for (int id = 1200; id <= kModelDetailsHeading; ++id) {
        const auto child = GetDlgItem(modelCenterWindow_, id);
        const bool heading = id == kModelListHeading || id == kModelDetailsHeading;
        const auto font = heading && modelCenterHeadingFont_ ? modelCenterHeadingFont_ : modelCenterFont_;
        if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    RECT client{};
    GetClientRect(modelCenterWindow_, &client);
    const int margin = px(16), gap = px(12);
    const int width = std::max(1, static_cast<int>(client.right) - margin * 2);
    const bool narrow = width < px(900);
    const int buttonHeight = px(34);
    const int buttonRows = 2;
    const int buttonsTop = std::max(px(180), static_cast<int>(client.bottom) - margin - buttonRows * (buttonHeight + gap) + gap);
    const int headingTop = px(120), headingHeight = px(20);
    const int contentTop = headingTop + headingHeight + px(6);
    const int contentBottom = std::max(contentTop + px(60), buttonsTop - gap);
    const int contentHeight = std::max(px(60), contentBottom - contentTop);
    const auto move = [&](HWND child, int x, int y, int w, int h) {
        if (child) SetWindowPos(child, nullptr, x, y, std::max(1, w), std::max(1, h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    move(modelCenterStatus_, margin, px(12), width, px(56));
    const int downloadButtonWidth = px(148);
    const int downloadButtonLeft = margin + width - downloadButtonWidth;
    move(modelDownloadUrl_, margin, px(76), width - downloadButtonWidth - gap, px(34));
    move(GetDlgItem(modelCenterWindow_, kModelDownload), downloadButtonLeft, px(76), downloadButtonWidth, px(34));
    if (narrow) {
        const int listHeight = std::max(px(48), contentHeight * 2 / 5);
        const int detailsHeadingTop = contentTop + listHeight + gap;
        const int detailsTop = detailsHeadingTop + headingHeight + px(4);
        move(modelListHeading_, margin, headingTop, width, headingHeight);
        move(modelList_, margin, contentTop, width, listHeight);
        move(modelDetailsHeading_, margin, detailsHeadingTop, width, headingHeight);
        move(modelDetails_, margin, detailsTop, width, contentBottom - detailsTop);
    } else {
        const int listWidth = width * 38 / 100;
        move(modelListHeading_, margin, headingTop, listWidth, headingHeight);
        move(modelDetailsHeading_, margin + listWidth + gap, headingTop, width - listWidth - gap, headingHeight);
        move(modelList_, margin, contentTop, listWidth, contentHeight);
        move(modelDetails_, margin + listWidth + gap, contentTop, width - listWidth - gap, contentHeight);
    }
    const std::array<int, 7> buttons{kModelRegister, kModelInstall, kModelCancelInstall, kModelDefault,
        kModelUnregister, kModelRefresh, kModelClose};
    const int columns = 4;
    const int buttonWidth = (width - (columns - 1) * gap) / columns;
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        const auto row = index / static_cast<std::size_t>(columns);
        const auto column = index % static_cast<std::size_t>(columns);
        const auto itemsOnRow = std::min<std::size_t>(static_cast<std::size_t>(columns), buttons.size() - row * static_cast<std::size_t>(columns));
        const int rowWidth = static_cast<int>(itemsOnRow) * buttonWidth + (static_cast<int>(itemsOnRow) - 1) * gap;
        const int rowLeft = margin + (width - rowWidth) / 2;
        move(GetDlgItem(modelCenterWindow_, buttons[index]), rowLeft + static_cast<int>(column) * (buttonWidth + gap),
            buttonsTop + static_cast<int>(row) * (buttonHeight + gap), buttonWidth, buttonHeight);
    }
}

void NativeApp::showModelCenter() {
    if (modelCenterWindow_ && IsWindow(modelCenterWindow_)) {
        ShowWindow(modelCenterWindow_, SW_SHOWNORMAL);
        SetForegroundWindow(modelCenterWindow_);
        updateModelDetails();
        return;
    }
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance_;
    cls.lpfnWndProc = &NativeApp::modelCenterWindowProcedure;
    cls.lpszClassName = L"RifeLoveLocalModelCenter";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    const auto scale = dpiScale();
    RECT owner{};
    if (window_) GetWindowRect(window_, &owner);
    RECT requested{owner.left + scaled(32, scale), owner.top + scaled(32, scale),
        owner.left + scaled(1012, scale), owner.top + scaled(652, scale)};
    requested = fitWindowToWorkArea(requested, monitorWorkArea(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST)));
    modelCenterWindow_ = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"RifeLove · 本地模型中心",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, requested.left, requested.top, requested.right - requested.left,
        requested.bottom - requested.top, window_, nullptr, instance_, this);
    if (modelCenterWindow_) {
        ShowWindow(modelCenterWindow_, SW_SHOWNORMAL);
        SetFocus(modelList_);
    }
}

void NativeApp::updateModelCenter() {
    if (!modelList_) return;
    SendMessageW(modelList_, LB_RESETCONTENT, 0, 0);
    int selection = 0;
    int extent = 0;
    const HDC dc = GetDC(modelList_);
    const auto oldFont = dc && modelCenterFont_ ? SelectObject(dc, modelCenterFont_) : nullptr;
    for (std::size_t index = 0; index < localModels_.size(); ++index) {
        const auto& item = localModels_[index];
        auto label = item.name + L"  " + item.version;
        if (item.modelSha256.size() >= 8) label += L"  [SHA " + item.modelSha256.substr(0, 8) + L"]";
        const bool selectedDefault = item.kind >= 0 && !modelPreferences_.defaults[static_cast<std::size_t>(item.kind)].empty() &&
            NativeModelPreferences::key(item.directory) == NativeModelPreferences::key(modelPreferences_.defaults[static_cast<std::size_t>(item.kind)]);
        if (selectedDefault) label += L"  [默认]";
        label += item.usable ? L"  [可选]" : item.verified ? L"  [能力未支持]" : L"  [校验未通过]";
        SendMessageW(modelList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (dc) { SIZE size{}; GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size); extent = std::max(extent, static_cast<int>(size.cx) + 24); }
        if (!modelCenterSelectedPath_.empty() && NativeModelPreferences::key(item.directory) == NativeModelPreferences::key(modelCenterSelectedPath_))
            selection = static_cast<int>(index);
    }
    if (dc) { if (oldFont) SelectObject(dc, oldFont); ReleaseDC(modelList_, dc); }
    SendMessageW(modelList_, LB_SETHORIZONTALEXTENT, extent, 0);
    SendMessageW(modelList_, LB_SETCURSEL, localModels_.empty() ? -1 : selection, 0);
    updateModelDetails();
}

void NativeApp::updateModelDetails() {
    if (!modelCenterWindow_ || !modelList_) return;
    const auto index = static_cast<std::size_t>(SendMessageW(modelList_, LB_GETCURSEL, 0, 0));
    const LocalModel* item = index < localModels_.size() ? &localModels_[index] : nullptr;
    const bool editable = modelPreferencesWritable_ && modelChangesAllowed();
    const bool installing = modelInstallActive_.load();
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelRegister), editable && !installing);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelInstall), editable && !installing);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelDownload), editable && !installing);
    EnableWindow(modelDownloadUrl_, editable && !installing);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelCancelInstall), installing);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelDefault), editable && item && item->usable);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelUnregister), editable && item);
    EnableWindow(GetDlgItem(modelCenterWindow_, kModelRefresh), modelChangesAllowed());
    std::wstring status = std::to_wstring(localModels_.size()) + L" 个模型。支持本地安装与 HTTPS 镜像下载。\r\n";
    status += L"SHA-256 检查文件完整性，不认证发布者或画质。\r\n";
    if (installing) {
        std::lock_guard lock(modelInstallMutex_);
        status += modelInstallState_.status;
        if (modelInstallState_.totalBytes != 0) {
            const auto percent = std::min<std::uint64_t>(100, modelInstallState_.completedBytes * 100 / modelInstallState_.totalBytes);
            status += L" · " + std::to_wstring(percent) + L"%";
        }
    } else status += modelChangesAllowed() ? modelCenterNotice_ : L"正在检查视频或运行任务；模型更改暂不可用。";
    SetWindowTextW(modelCenterStatus_, status.c_str());
    std::wstring details = L"暂无已登记模型。\r\n\r\n可在上方填入 1–5 个 HTTPS ZIP 镜像地址并选择“下载并安装”；每个地址旁需能取得同名 .distribution.json 清单。中断下载会保留可续传数据。也可安装本地 ZIP 或登记已解压的本机目录。";
    if (item) {
        modelCenterSelectedPath_ = item->directory;
        const wchar_t* kind = item->kind == 0 ? L"RIFE 插帧" : item->kind == 1 ? L"超分辨率" : item->kind == 2 ? L"时域降噪" : item->kind == 3 ? L"压缩修复 · STDF 本机评估" : item->kind == 4 ? L"实验性独立去模糊" : L"其他 / 尚未支持";
        details = L"名称：" + item->name + L"\r\n版本：" + item->version + L"\r\n类别：" + kind;
        details += L"\r\n\r\n能力：" + item->capabilities + L"\r\n颜色 / 输入配置：" + item->profiles;
        details += L"\r\n\r\n验证状态：" + item->verification;
        details += item->usable ? L"\r\n当前客户端可选用；实际处理仍需通过运行库、设备和源视频检查。" : L"\r\n仅可查看，不能设为默认。";
        details += L"\r\n\r\n模型 SHA-256：" + (item->modelSha256.empty() ? L"未声明" : item->modelSha256);
        details += L"\r\n许可：" + (item->licenseId.empty() ? L"未声明" : item->licenseId);
        details += L"\r\n来源仓库：" + (item->sourceRepository.empty() ? L"未声明" : item->sourceRepository);
        details += L"\r\n来源提交：" + (item->upstreamCommit.empty() ? L"未固定" : item->upstreamCommit);
        details += L"\r\n验证配置：" + (item->numericalProfile.empty() ? L"未声明" : item->numericalProfile);
        details += L"\r\n验证后端：" + (item->validatedBackends.empty() ? L"未声明" : item->validatedBackends);
        details += L"\r\n许可范围证据：" + (item->licenseScopeEvidence.empty() ? L"未提供" : item->licenseScopeEvidence);
        details += L"\r\n\r\nSHA-256 与侧车文件哈希用于检查本地文件一致性，不认证发布者身份，也不代表画质排名。";
        details += L"\r\n\r\n完整目录（可选择并复制）：\r\n" + item->directory.wstring();
        details += L"\r\n\r\n默认项按类别分别保存；实验性去模糊启动时始终关闭，需在主窗口显式开启；可与超分同时选择。";
    }
    details += L"\r\n\r\n用户数据目录：\r\n" + dataDirectory_.directory.wstring();
    details += L"\r\n" + dataDirectory_.notice;
    details += L"\r\n旧版数据目录（原件保留）：\r\n" + dataDirectory_.legacyDirectory.wstring();
    if (controlText(modelDetails_) != details) SetWindowTextW(modelDetails_, details.c_str());
}

void NativeApp::setHistoryFont(HWND window) {
    if (historyFont_) DeleteObject(historyFont_);
    const UINT rawDpi = GetDpiForWindow(window);
    const int dpi = static_cast<int>(rawDpi ? rawDpi : 96U);
    historyFont_ = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    for (const auto control : {historyStatus_, historyRefreshButton_, historyList_, historyText_,
                               historyRetryButton_, historyResumeButton_, historyRestartButton_}) {
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(historyFont_), TRUE);
    }
}

LRESULT CALLBACK NativeApp::historyWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    const UINT rawDpi = GetDpiForWindow(window);
    const float scale = static_cast<float>(rawDpi ? rawDpi : 96U) / 96.0F;
    const auto px = [&](int value) { return scaled(value, scale); };
    switch (message) {
    case WM_CREATE:
        app->historyStatus_ = CreateWindowExW(0, L"STATIC", L"准备读取本地任务记录…",
            WS_CHILD | WS_VISIBLE | SS_LEFT, px(16), px(14), px(560), px(24), window, nullptr, app->instance_, nullptr);
        app->historyRefreshButton_ = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, px(600), px(10), px(110), px(34), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryRefresh)), app->instance_, nullptr);
        app->historyList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            px(16), px(50), px(694), px(145), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryList)), app->instance_, nullptr);
        app->historyText_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            px(16), px(205), px(694), px(255), window, nullptr, app->instance_, nullptr);
        app->historyRetryButton_ = CreateWindowExW(0, L"BUTTON", L"重试失败任务",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, px(16), px(472), px(140), px(34), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryRetry)), app->instance_, nullptr);
        app->historyResumeButton_ = CreateWindowExW(0, L"BUTTON", L"断点继续",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, px(166), px(472), px(140), px(34), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryResume)), app->instance_, nullptr);
        app->historyRestartButton_ = CreateWindowExW(0, L"BUTTON", L"从头另存重做",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, px(316), px(472), px(160), px(34), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHistoryRestart)), app->instance_, nullptr);
        if (app->historyText_) {
            SendMessageW(app->historyText_, EM_SETLIMITTEXT, static_cast<WPARAM>(8U * 1024U * 1024U), 0);
        }
        app->setHistoryFont(window);
        app->updateTaskHistorySelection();
        return 0;
    case WM_SIZE: {
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (app->historyStatus_) SetWindowPos(app->historyStatus_, nullptr, px(16), px(14),
            std::max(px(120), width - px(150)), px(24), SWP_NOZORDER | SWP_NOACTIVATE);
        if (app->historyRefreshButton_) SetWindowPos(app->historyRefreshButton_, nullptr,
            std::max(px(16), width - px(126)), px(10), px(110), px(34), SWP_NOZORDER | SWP_NOACTIVATE);
        if (app->historyList_) SetWindowPos(app->historyList_, nullptr, px(16), px(50),
            std::max(px(180), width - px(32)), px(145), SWP_NOZORDER | SWP_NOACTIVATE);
        if (app->historyText_) SetWindowPos(app->historyText_, nullptr, px(16), px(205),
            std::max(px(180), width - px(32)), std::max(px(60), height - px(265)), SWP_NOZORDER | SWP_NOACTIVATE);
        const int actionTop = std::max(px(274), height - px(46));
        for (const auto& [button, left, buttonWidth] : std::array{
                 std::tuple{app->historyRetryButton_, 16, 140},
                 std::tuple{app->historyResumeButton_, 166, 140},
                 std::tuple{app->historyRestartButton_, 316, 160}}) {
            if (button) SetWindowPos(button, nullptr, px(left), actionTop, px(buttonWidth), px(34),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (info) {
            RECT minimum{0, 0, px(560), px(320)};
            const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
            if (AdjustWindowRectExForDpi(&minimum, style, FALSE, extendedStyle, rawDpi ? rawDpi : 96U)) {
                const RECT work = monitorWorkArea(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
                info->ptMinTrackSize.x = std::min<LONG>(minimum.right - minimum.left, work.right - work.left);
                info->ptMinTrackSize.y = std::min<LONG>(minimum.bottom - minimum.top, work.bottom - work.top);
            } else {
                info->ptMinTrackSize.x = px(560);
                info->ptMinTrackSize.y = px(352);
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            const RECT work = monitorWorkArea(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
            const RECT fitted = fitWindowToWorkArea(*suggested, work);
            SetWindowPos(window, nullptr, fitted.left, fitted.top, fitted.right - fitted.left,
                fitted.bottom - fitted.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        app->setHistoryFont(window);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kHistoryRefresh && HIWORD(wParam) == BN_CLICKED) {
            app->refreshTaskHistory();
            return 0;
        }
        if (LOWORD(wParam) == kHistoryList && HIWORD(wParam) == LBN_SELCHANGE) {
            app->updateTaskHistorySelection();
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            if (LOWORD(wParam) == kHistoryRetry) app->operateOnHistoryTask(HistoryAction::retry);
            else if (LOWORD(wParam) == kHistoryResume) app->operateOnHistoryTask(HistoryAction::resumeCheckpoint);
            else if (LOWORD(wParam) == kHistoryRestart) app->operateOnHistoryTask(HistoryAction::restartFromZero);
            else break;
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!item || (item->CtlID != kHistoryRefresh && item->CtlID != kHistoryRetry &&
                      item->CtlID != kHistoryResume && item->CtlID != kHistoryRestart)) break;
        const ScopedDcState saved(item->hDC);
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const COLORREF fill = disabled ? RGB(38, 49, 63) : pressed ? RGB(42, 72, 91) : RGB(34, 56, 82);
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1)), disabled ? RGB(48, 61, 76) : RGB(67, 91, 111));
        const auto oldBrush = SelectObject(item->hDC, brush);
        const auto oldPen = SelectObject(item->hDC, pen);
        RECT rect = item->rcItem;
        if (pressed) InflateRect(&rect, -px(1), -px(1));
        RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, px(8), px(8));
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, disabled ? RGB(125, 140, 157) : RGB(242, 247, 255));
        if (app->historyFont_) SelectObject(item->hDC, app->historyFont_);
        std::wstring label = app->controlText(item->hwndItem);
        DrawTextW(item->hDC, label.c_str(), static_cast<int>(label.size()), &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if ((item->itemState & ODS_FOCUS) != 0) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -px(3), -px(3));
            DrawFocusRect(item->hDC, &focus);
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(159, 178, 201));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(12, 15, 21));
        return reinterpret_cast<LRESULT>(app->backgroundBrush_);
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 247, 255));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(14, 23, 34));
        return reinterpret_cast<LRESULT>(app->editBrush_);
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 247, 255));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(14, 23, 34));
        return reinterpret_cast<LRESULT>(app->editBrush_);
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 247, 255));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(19, 24, 32));
        return reinterpret_cast<LRESULT>(app->editBrush_);
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, app->backgroundBrush_);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        if (app->historyWindow_ == window) app->historyWindow_ = nullptr;
        app->historyList_ = nullptr;
        app->historyText_ = nullptr;
        app->historyStatus_ = nullptr;
        app->historyRefreshButton_ = nullptr;
        app->historyRetryButton_ = nullptr;
        app->historyResumeButton_ = nullptr;
        app->historyRestartButton_ = nullptr;
        if (app->historyFont_) { DeleteObject(app->historyFont_); app->historyFont_ = nullptr; }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void NativeApp::showTaskHistory() {
    if (!taskQueue_) return;
    if (historyWindow_ && IsWindow(historyWindow_)) {
        ShowWindow(historyWindow_, SW_SHOWNORMAL);
        SetForegroundWindow(historyWindow_);
        refreshTaskHistory();
        return;
    }
    static std::once_flag registration;
    std::call_once(registration, [this] {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance_;
        cls.lpfnWndProc = &NativeApp::historyWindowProcedure;
        cls.lpszClassName = L"RifeLoveTaskHistoryWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = backgroundBrush_;
        cls.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&cls);
    });
    const float scale = dpiScale();
    RECT requested{0, 0, scaled(760, scale), scaled(520, scale)};
    if (window_) {
        RECT owner{};
        GetWindowRect(window_, &owner);
        requested.left = owner.left + scaled(46, scale);
        requested.top = owner.top + scaled(46, scale);
        requested.right = requested.left + scaled(760, scale);
        requested.bottom = requested.top + scaled(520, scale);
    }
    const RECT work = monitorWorkArea(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
    requested = fitWindowToWorkArea(requested, work);
    historyWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"RifeLoveTaskHistoryWindow", L"RifeLove · 任务历史",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        requested.left, requested.top, requested.right - requested.left, requested.bottom - requested.top,
        window_, nullptr, instance_, this);
    if (!historyWindow_) return;
    ShowWindow(historyWindow_, SW_SHOWNORMAL);
    UpdateWindow(historyWindow_);
    refreshTaskHistory();
}

void NativeApp::showComparison() {
    if (!comparisonWindow_) comparisonWindow_ = std::make_unique<NativeComparisonWindow>(NativeComparisonWindow::Loader{},
        dataDirectory_.writable ? dataDirectory_.directory : std::filesystem::path{});
    if (!comparisonWindow_->open(window_, currentSource(), currentOutput()))
        MessageBoxW(window_, L"无法创建本地逐帧比较窗口。", L"RifeLove", MB_OK | MB_ICONERROR);
}

bool NativeApp::automaticDenoiseAvailable() const {
    const int index = static_cast<int>(SendMessageW(temporalRestorationCombo_, CB_GETCURSEL, 0, 0));
    return index > 0 && index <= static_cast<int>(temporalRestorationModels_.size()) &&
        isTemporalRestoration(temporalRestorationModels_[static_cast<std::size_t>(index - 1)]);
}

bool NativeApp::setAutomaticDenoise(bool enabled) {
    if (!modelChangesAllowed()) return false;
    if (enabled && !automaticDenoiseAvailable()) return false;
    automaticTemporalDenoise_ = enabled;
    updateView();
    return true;
}

void NativeApp::configureAutomaticDenoise() {
    if (!modelChangesAllowed()) return;
    if (!automaticDenoiseAvailable()) {
        MessageBoxW(window_, L"请先在可选降噪中选择已校验的 FastDVDnet。\n\n实验性自动高斯降噪默认关闭，不是压缩修复或一般细节恢复。",
            L"实验性自动高斯降噪", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int choice = MessageBoxW(window_,
        L"实验性自动高斯降噪仅估计近似高斯噪声并调节 FastDVDnet。\n\n"
        L"可能将胶片颗粒误判为噪声并损失细节；不能修复压缩伪影，也不等于一般细节恢复。\n"
        L"当前实现强制软件解码；断点恢复时需要从头重放。默认关闭。\n\n"
        L"是：启用实验性自动模式（可独立设置相对偏移；0 跟随自动）\n否：使用固定 σ25（仍降噪）\n取消：保持当前设置\n\n"
        L"若要完全关闭降噪，请在“可选降噪”中选择关闭。",
        L"实验性自动高斯降噪", MB_YESNOCANCEL | MB_ICONINFORMATION | (automaticTemporalDenoise_ ? MB_DEFBUTTON1 : MB_DEFBUTTON2));
    if (choice == IDYES || choice == IDNO) (void)setAutomaticDenoise(choice == IDYES);
}

bool NativeApp::automaticCompressionAvailable() const {
    const int model = static_cast<int>(SendMessageW(compressionRestorationCombo_, CB_GETCURSEL, 0, 0));
    const int strength = static_cast<int>(SendMessageW(compressionStrengthCombo_, CB_GETCURSEL, 0, 0));
    return model > 0 && model <= static_cast<int>(compressionRestorationModels_.size()) && strength > 0 && strength <= 3 &&
        restoration::supportsStdfCudaModel(compressionRestorationModels_[static_cast<std::size_t>(model - 1)]);
}

bool NativeApp::automaticDetailAvailable() const {
    if (!deblurRestorationCombo_) return false;
    const int index = static_cast<int>(SendMessageW(deblurRestorationCombo_, CB_GETCURSEL, 0, 0));
    return index > 0 && index <= static_cast<int>(deblurRestorationModels_.size()) &&
        restoration::supportsOnnxCudaOutputRateModel(deblurRestorationModels_[static_cast<std::size_t>(index - 1)]) &&
        restoration::outputRateImageTask(deblurRestorationModels_[static_cast<std::size_t>(index - 1)].capabilities) == restoration::Task::deblur;
}

bool NativeApp::setAutomaticDetail(bool enabled) {
    if (!modelChangesAllowed() || (enabled && !automaticDetailAvailable())) return false;
    if (enabled && motionBlurCheck_ && SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        try {
            if (parsePositiveInteger(controlText(shutterEdit_), "shutter angle") > 0 &&
                parsePositiveInteger(controlText(samplesEdit_), "sample count") > 1) return false;
        } catch (const std::exception&) { return false; }
    }
    automaticDetailRestoration_ = enabled;
    updateView();
    return true;
}

void NativeApp::configureAutomaticDetail() {
    if (!modelChangesAllowed()) return;
    if (!automaticDetailAvailable()) {
        MessageBoxW(window_, L"请先在独立去模糊栏选择已校验的同尺寸 NAFNet 模型。\n\n关闭自动细节时，已选模型仍会以固定强度去模糊；完全关闭请将模型改为“关闭”。",
            L"实验性自动细节", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int choice = MessageBoxW(window_,
        L"实验性自动细节逐帧估计模糊程度，调节独立 NAFNet 去模糊强度。\n\n"
        L"只验证了受支持的 SDR8 输入；可能产生纹理过锐、边缘伪影或错误细节。"
        L"额外模型推理、软件解码与高分辨率分块会增加耗时和显存占用。\n"
        L"与自然动态曝光合成互斥；若已开启有效曝光，请先关闭曝光。默认关闭。\n\n"
        L"是：启用自动细节（0% 跟随估计）\n否：保留固定去模糊\n取消：不更改设置",
        L"实验性自动细节", MB_YESNOCANCEL | MB_ICONINFORMATION |
            (automaticDetailRestoration_ ? MB_DEFBUTTON1 : MB_DEFBUTTON2));
    if (choice == IDYES || choice == IDNO) {
        if (!setAutomaticDetail(choice == IDYES) && choice == IDYES)
            MessageBoxW(window_, L"自动细节与当前自然动态曝光设置冲突。请关闭自然动态曝光后重试。",
                L"实验性自动细节", MB_OK | MB_ICONWARNING);
    }
}

bool NativeApp::setAutomaticCompression(bool enabled) {
    if (!modelChangesAllowed() || (enabled && !automaticCompressionAvailable())) return false;
    automaticCompressionRestoration_ = enabled;
    updateView();
    return true;
}

void NativeApp::configureAutomaticCompression() {
    if (!modelChangesAllowed()) return;
    if (!automaticCompressionAvailable()) {
        MessageBoxW(window_, L"请先选择已校验 STDF 模型并将手动强度设为 25%、50% 或 100%。\n\n自动模式会独立分析源亮度块状不连续；默认关闭，不等于通用压缩修复或细节增强。",
            L"实验性自动压缩修复", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const int choice = MessageBoxW(window_,
        L"实验性自动压缩修复只检测原始 SDR8 亮度中的分布式 8 像素块状不连续，逐源帧调节 STDF 混合强度。\n\n"
        L"纹理可能误判；低置信度帧直接跳过。它不是编码器/QP 检测器，也不是细节增强模型。\n"
        L"强制软件解码；恢复任务需从原始时间轴开头重放。默认关闭。\n\n"
        L"是：启用自动（偏移 0 跟随估计，−100% 更轻，+100% 更强）\n"
        L"否：返回所选手动强度\n取消：保持当前设置",
        L"实验性自动压缩修复", MB_YESNOCANCEL | MB_ICONINFORMATION | (automaticCompressionRestoration_ ? MB_DEFBUTTON1 : MB_DEFBUTTON2));
    if (choice == IDYES || choice == IDNO) (void)setAutomaticCompression(choice == IDYES);
}

void NativeApp::refreshTaskHistory() {
    if (!historyWindow_ || !IsWindow(historyWindow_)) return;
    const auto generation = ++historyGeneration_;
    historyLoading_.store(true);
    if (historyStatus_) SetWindowTextW(historyStatus_, L"正在读取本地 SQLite 任务记录…");
    historyCheckpointCandidateTaskIds_.clear();
    historyCheckpointInspectionError_.clear();
    historyRows_.clear();
    if (historyList_) SendMessageW(historyList_, LB_RESETCONTENT, 0, 0);
    if (historyText_) SetWindowTextW(historyText_, L"正在读取…");
    updateTaskHistorySelection();
    historyThread_.request_stop();
    if (historyThread_.joinable()) historyThread_.join();
    const auto callback = callbackWindow_.load();
    historyThread_ = std::jthread([this, generation, callback](std::stop_token stop) {
        auto* result = new NativeHistoryResult{};
        result->generation = generation;
        try {
            if (stop.stop_requested() || closing_) { delete result; return; }
            if (!taskQueue_) throw std::runtime_error("本地任务队列不可用");
            result->tasks = taskQueue_->list();
            for (auto& task : result->tasks) {
                if (stop.stop_requested() || closing_) { delete result; return; }
                if (task.task.state != TaskState::interrupted) continue;
                try {
                    if (taskQueue_->hasCommittedCheckpointSegments(task.task.id))
                        result->checkpointCandidateTaskIds.push_back(task.task.id);
                } catch (const std::exception& error) {
                    result->checkpointInspectionError = error.what();
                    break;
                }
            }
            if (stop.stop_requested() || closing_) { delete result; return; }
            result->status = result->tasks.empty() ? L"没有任务记录" :
                L"已读取 " + std::to_wstring(result->tasks.size()) + L" 条记录；最新任务已选中，可查看详情与可用操作";
            if (!result->checkpointInspectionError.empty())
                result->status += L"；检查点状态读取失败，断点继续暂不可用";
        } catch (const std::exception& error) {
            result->status = L"无法读取本地任务记录：" + wideFromUtf8(error.what());
        }
        const auto target = callbackWindow_.load();
        if (!target || closing_ || !PostMessageW(target, kHistoryLoadedMessage, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    });
}

void NativeApp::handleHistoryResult(NativeHistoryResult result) {
    if (result.generation != historyGeneration_) return;
    historyLoading_.store(false);
    // The queue snapshot is FIFO because the worker consumes it in sequence
    // order. Present its separate history view by recency, retaining the later
    // enqueue order when SQLite's second-resolution timestamps tie.
    std::reverse(result.tasks.begin(), result.tasks.end());
    std::stable_sort(result.tasks.begin(), result.tasks.end(), [](const ConfiguredTask& left, const ConfiguredTask& right) {
        return left.task.updatedAt > right.task.updatedAt;
    });
    historyRows_ = std::move(result.tasks);
    historyCheckpointCandidateTaskIds_ = std::move(result.checkpointCandidateTaskIds);
    historyCheckpointInspectionError_ = std::move(result.checkpointInspectionError);
    if (historyList_ && IsWindow(historyList_)) {
        SendMessageW(historyList_, LB_RESETCONTENT, 0, 0);
        for (const auto& row : historyRows_) {
            const auto label = taskHistoryRowLabel(row);
            SendMessageW(historyList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!historyRows_.empty()) SendMessageW(historyList_, LB_SETCURSEL, 0, 0);
    }
    if (historyStatus_ && IsWindow(historyStatus_)) SetWindowTextW(historyStatus_, result.status.c_str());
    updateTaskHistorySelection();
}

void NativeApp::updateTaskHistorySelection() {
    const LRESULT selection = historyList_ ? SendMessageW(historyList_, LB_GETCURSEL, 0, 0) : LB_ERR;
    const bool selected = selection != LB_ERR && selection >= 0 &&
        static_cast<std::size_t>(selection) < historyRows_.size();
    const ConfiguredTask* selectedTask = selected ? &historyRows_[static_cast<std::size_t>(selection)] : nullptr;
    const bool hasCheckpoint = selectedTask && historyCheckpointInspectionError_.empty() &&
        std::find(historyCheckpointCandidateTaskIds_.begin(), historyCheckpointCandidateTaskIds_.end(),
            selectedTask->task.id) != historyCheckpointCandidateTaskIds_.end();
    if (historyText_ && IsWindow(historyText_)) {
        const std::wstring details = selected
            ? taskHistoryText(*selectedTask, historyCheckpointInspectionError_.empty(), hasCheckpoint)
            : historyRows_.empty() ? L"暂无任务记录。完成或提交一次插帧后，记录会显示在上方列表。"
                                   : L"选择上方任务可查看源文件、输出文件、时间和错误详情。";
        SetWindowTextW(historyText_, details.c_str());
    }
    const bool idle = !historyLoading_.load() && !queueSessionActive_.load();
    const auto state = selected ? historyRows_[static_cast<std::size_t>(selection)].task.state : TaskState::queued;
    if (historyRetryButton_) EnableWindow(historyRetryButton_, idle && selected && state == TaskState::failed);
    if (historyResumeButton_) EnableWindow(historyResumeButton_, idle && selected && state == TaskState::interrupted && hasCheckpoint);
    if (historyRestartButton_) EnableWindow(historyRestartButton_, idle && selected && state == TaskState::interrupted);
    if (historyRefreshButton_) EnableWindow(historyRefreshButton_, !historyLoading_.load());
}

void NativeApp::operateOnHistoryTask(HistoryAction action) {
    if (!taskQueue_ || queueSessionActive_.load() || historyLoading_.load()) return;
    const LRESULT selection = historyList_ ? SendMessageW(historyList_, LB_GETCURSEL, 0, 0) : LB_ERR;
    if (selection == LB_ERR || selection < 0 || static_cast<std::size_t>(selection) >= historyRows_.size()) return;
    const std::string taskId = historyRows_[static_cast<std::size_t>(selection)].task.id;

    std::vector<ConfiguredTask> rows;
    try {
        rows = taskQueue_->list();
    } catch (const std::exception& error) {
        MessageBoxW(historyWindow_, (L"无法读取当前队列状态：" + wideFromUtf8(error.what())).c_str(),
            L"RifeLove 任务历史", MB_OK | MB_ICONERROR);
        return;
    }
    const auto current = std::find_if(rows.begin(), rows.end(), [&](const ConfiguredTask& row) {
        return row.task.id == taskId;
    });
    if (current == rows.end()) {
        refreshTaskHistory();
        return;
    }
    const auto expectedState = action == HistoryAction::retry ? TaskState::failed : TaskState::interrupted;
    if (current->task.state != expectedState) {
        refreshTaskHistory();
        MessageBoxW(historyWindow_, L"该任务状态已变化，请刷新记录后重试。", L"RifeLove 任务历史",
            MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::filesystem::path restartOutput;
    if (action == HistoryAction::restartFromZero) {
        try {
            restartOutput = restartedOutputPath(*current, rows);
        } catch (const std::exception& error) {
            MessageBoxW(historyWindow_, (L"无法为重做任务准备新输出：\r\n" + wideFromUtf8(error.what())).c_str(),
                L"RifeLove 任务历史", MB_OK | MB_ICONERROR);
            return;
        }
        const auto prompt = L"这会保留原任务记录，并从头处理到新的输出文件：\r\n\r\n" + restartOutput.wstring() +
            L"\r\n\r\n是否继续？";
        if (MessageBoxW(historyWindow_, prompt.c_str(), L"从头重做任务", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
    }

    const auto queued = static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(), [](const ConfiguredTask& row) {
        return row.task.state == TaskState::queued;
    }));
    if (queued != 0) {
        const auto prompt = L"队列中已有 " + std::to_wstring(queued) +
            L" 个待处理任务。此操作会按顺序处理队列中的任务。\r\n\r\n是否继续？";
        if (MessageBoxW(historyWindow_, prompt.c_str(), L"运行本地任务队列",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }

    if (queueOwnerThread_.joinable()) queueOwnerThread_.join();
    queueImportStop_ = std::stop_source{};
    queueAccepted_ = 0;
    queueRejected_ = 0;
    queueHadFailures_ = false;
    queuePaused_ = false;
    queuePauseTransitionPending_ = false;
    queueFirstRejection_.clear();
    queueRejections_.clear();
    queueSessionActive_.store(true);
    const std::wstring actionLabel = action == HistoryAction::retry ? L"重试失败任务" :
        action == HistoryAction::resumeCheckpoint ? L"校验并继续断点任务" : L"创建从头重做任务";
    queueOwnerThread_ = std::jthread([this, taskId, action, restartOutput, actionLabel](std::stop_token ownerStop) {
        std::stop_callback stopBridge(ownerStop, [this] {
            queueImportStop_.request_stop();
            if (taskQueue_) taskQueue_->stop();
        });
        try {
            if (!taskQueue_) throw std::runtime_error("本地任务队列不可用");
            if (ownerStop.stop_requested()) throw std::runtime_error("Queue cancelled");
            if (action == HistoryAction::retry) taskQueue_->retry(taskId);
            else if (action == HistoryAction::resumeCheckpoint) taskQueue_->resumeCheckpoint(taskId);
            else (void)taskQueue_->restart(taskId, utf8(restartOutput));
            if (ownerStop.stop_requested()) throw std::runtime_error("Queue cancelled");
            postQueueNotice({L"任务已加入本地队列，正在按顺序处理", 1, 0, true, false, false});
            if (!taskQueue_->start([this](application::QueueEvent event) {
                    postQueueEvent(std::move(event));
                })) throw std::runtime_error("本地任务队列已经在运行");
            taskQueue_->wait();
            postQueueNotice({L"", 1, 0, false, true, false});
        } catch (const std::exception& error) {
            const bool cancelled = ownerStop.stop_requested() || queueImportStop_.stop_requested();
            postQueueNotice({cancelled ? L"队列操作已取消；未完成任务保留在本地队列中" :
                    actionLabel + L"失败：" + wideFromUtf8(error.what()),
                0, 0, false, true, !cancelled});
        }
    });
    if (historyStatus_) SetWindowTextW(historyStatus_, actionLabel.c_str());
    updateTaskHistorySelection();
    updateView();
}
#endif

void NativeApp::applyExportControls(PipelineSettings& settings) const {
    settings.automaticTemporalDenoise.reset();
    settings.automaticCompressionRestoration.reset();
    settings.automaticDetailRestoration.reset();
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (automaticTemporalDenoise_) {
        if (!automaticDenoiseAvailable()) throw std::invalid_argument("experimental automatic Gaussian denoise requires a selected validated FastDVDnet model");
        settings.automaticTemporalDenoise.emplace();
        settings.automaticTemporalDenoise->relativeAdjustment = nativeRelativeAdjustment(controlText(denoiseAdjustmentEdit_));
    }
    if (automaticCompressionRestoration_) {
        if (!automaticCompressionAvailable()) throw std::invalid_argument("automatic compression restoration requires an enabled, qualified STDF model");
        settings.automaticCompressionRestoration.emplace();
        settings.automaticCompressionRestoration->relativeAdjustment = nativeRelativeAdjustment(controlText(compressionAdjustmentEdit_));
    }
    if (automaticDetailRestoration_) {
        if (!automaticDetailAvailable()) throw std::invalid_argument("automatic detail requires a selected validated same-resolution deblur model");
        settings.automaticDetailRestoration.emplace();
        settings.automaticDetailRestoration->relativeAdjustment = nativeRelativeAdjustment(controlText(detailAdjustmentEdit_));
    }
#endif
    const int rateIndex = static_cast<int>(SendMessageW(rateCombo_, CB_GETCURSEL, 0, 0));
    if (rateIndex >= 0 && rateIndex < static_cast<int>(kTargetRates.size()))
        settings.targetRate = Rational(kTargetRates[static_cast<std::size_t>(rateIndex)]);
    else if (rateIndex == static_cast<int>(kTargetRates.size()))
        settings.targetRate = Rational::parse(utf8FromWide(controlText(rateCustomEdit_)));
    else throw std::invalid_argument("select a target frame rate");
    if (settings.targetRate <= Rational(0)) throw std::invalid_argument("target frame rate must be positive");

    const std::array<const char*, 3> encoders{"auto", "h264_nvenc", "libopenh264"};
    const int encoderIndex = static_cast<int>(SendMessageW(encoderCombo_, CB_GETCURSEL, 0, 0));
    if (encoderIndex < 0 || encoderIndex >= static_cast<int>(encoders.size()))
        throw std::invalid_argument("select a video encoder");
    settings.encoding.encoder = encoders[static_cast<std::size_t>(encoderIndex)];
    settings.encoding.frameRate = settings.targetRate;
    settings.encoding.videoStreamIndex = -1;

    int scaleNumerator = 1;
    int scaleDenominator = 1;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    const int restorationIndex = static_cast<int>(SendMessageW(restorationCombo_, CB_GETCURSEL, 0, 0));
    if (restorationIndex > 0 && restorationIndex <= static_cast<int>(spatialRestorationModels_.size())) {
        const auto& restoration = spatialRestorationModels_[static_cast<std::size_t>(restorationIndex - 1)];
        scaleNumerator = restoration.capabilities.scaleNumerator;
        scaleDenominator = restoration.capabilities.scaleDenominator;
        if (isUpscaleRestoration(restoration)) {
            const auto target = outputTargetCheck(static_cast<int>(SendMessageW(outputSizeCombo_, CB_GETCURSEL, 0, 0)));
            if (target.usable()) {
                settings.encoding.outputWidth = target.width;
                settings.encoding.outputHeight = target.height;
            }
        }
    }
#endif
    const int platformIndex = static_cast<int>(SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0));
    if (platformIndex > 0
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        && !hasBatchSelection()
#endif
    ) {
        const auto profile = platformIndex == 1 ? application::PlatformBitrate::youtube : application::PlatformBitrate::localQuality;
        const auto platform = application::platformExportSettings(profile, settings.targetRate, sourceWidth_, sourceHeight_,
            settings.encoding.outputWidth, settings.encoding.outputHeight, scaleNumerator, scaleDenominator);
        settings.encoding.bitRate = static_cast<std::int64_t>(platform.bitRateKbps) * 1000;
        settings.encoding.gopSize = platform.gopSizeFrames;
    }
    settings.motionBlur.enabled = SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (settings.motionBlur.enabled) {
        settings.motionBlur.shutterAngleDegrees = parsePositiveInteger(controlText(shutterEdit_), "shutter angle");
        settings.motionBlur.sampleCount = parsePositiveInteger(controlText(samplesEdit_), "sample count");
        if (settings.motionBlur.shutterAngleDegrees < 0 || settings.motionBlur.shutterAngleDegrees > 360)
            throw std::invalid_argument("shutter angle must be 0..360 degrees");
        if (settings.motionBlur.sampleCount < 1 || settings.motionBlur.sampleCount > 16)
            throw std::invalid_argument("sample count must be 1..16");
    }
    if (settings.automaticDetailRestoration && settings.motionBlur.enabled &&
        settings.motionBlur.shutterAngleDegrees > 0 && settings.motionBlur.sampleCount > 1)
        throw std::invalid_argument("automatic detail conflicts with exposure-synthesis motion blur");
}

void NativeApp::applyPlatformPreset(int index) {
    if (index < 0 || index >= static_cast<int>(exportPresetControls_.size())) index = 0;
    // Each preset keeps the user's edits for this window session. Leaving a
    // platform copy cannot replace the independent high-rate master settings.
    auto& previous = exportPresetControls_[static_cast<std::size_t>(lastPlatformSelection_)];
    previous = {static_cast<int>(SendMessageW(rateCombo_, CB_GETCURSEL, 0, 0)),
        static_cast<int>(SendMessageW(encoderCombo_, CB_GETCURSEL, 0, 0)), controlText(rateCustomEdit_),
        SendMessageW(motionBlurCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED,
        controlText(shutterEdit_), controlText(samplesEdit_), true};
    auto& selected = exportPresetControls_[static_cast<std::size_t>(index)];
    if (!selected.saved) {
        selected.rateSelection = index == 3 ? 2 : index == 4 ? 0 : 1;
        selected.blurEnabled = index == 1 || index == 2 || index == 5;
    }
    SendMessageW(rateCombo_, CB_SETCURSEL, selected.rateSelection, 0);
    SendMessageW(encoderCombo_, CB_SETCURSEL, selected.encoderSelection, 0);
    SetWindowTextW(rateCustomEdit_, selected.customRate.c_str());
    SendMessageW(motionBlurCheck_, BM_SETCHECK, selected.blurEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(shutterEdit_, selected.shutterAngle.c_str());
    SetWindowTextW(samplesEdit_, selected.sampleCount.c_str());
    lastPlatformSelection_ = index;
    if (index == 0) platformSummary_ = L"本机母版：自定义帧率、编码器与曝光独立保存";
    else if (index == 1) platformSummary_ = L"YouTube 本地副本：SDR 码率参考，可手动修改";
    else if (index == 2) platformSummary_ = L"Bilibili 本地副本：码率为本机兼容性建议";
    else if (index == 3) platformSummary_ = L"Bilibili 历史 120 FPS 实验；当前权益需账号实测";
    else platformSummary_ = L"抖音本地副本：本机兼容性建议，投稿规则需另核对";
    const auto source = currentSource();
    if (
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        !hasBatchSelection()
#else
        true
#endif
    ) {
        auto output = currentOutput();
        if (output.empty() && !source.empty()) output = source.parent_path() / (source.stem().wstring() + L"_RifeLove");
        if (!output.empty()) {
            output.replace_extension(index > 0 ? L".mp4" : L".mkv");
            setOutputPath(output);
        }
    }
    state_.status = index == 0 ? L"已选择本机高帧率母版" : L"已选择本地平台副本预设；RifeLove 不会上传文件";
    layoutControls();
    refreshReadinessStatus();
    updateView();
}

void NativeApp::startProcessing() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    const auto readiness = evaluateStartReadiness();
    if (!readiness.ready) {
        state_.status = readiness.status;
        updateView();
        return;
    }
    const auto source = currentSource();
    const auto output = currentOutput();
    application::ProcessingJob job;
    job.source = utf8(source);
    job.output = utf8(output);
    job.modelDirectory = modelDirectory_;
    job.runtimeDirectory = runtimeDirectory_;
    if (!captureSelectedRestoration(job)) return;
    const int platformIndex = static_cast<int>(SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0));
    try {
        applyExportControls(job.settings);
    } catch (const std::exception& error) {
        state_.status = L"导出设置无效：" + wideFromUtf8(error.what());
        updateView();
        return;
    }
    job.settings.inferenceBackend = "auto";
    job.expectedModelId = modelPack_->id;
    job.expectedModelVersion = modelPack_->version;
    job.expectedModelSha256 = modelPack_->sha256;
    job.expectedModelAdapterId = modelPack_->adapterId;
    job.expectedModelAdapterVersion = modelPack_->adapterVersion;
    const bool batch = hasBatchSelection();
    if (batch) {
        // ImportService probes every source independently. Keep the base job
        // free of the first video's dimensions and platform bitrate so a
        // mixed-resolution batch cannot silently inherit them.
        job.settings.encoding.outputWidth = 0;
        job.settings.encoding.outputHeight = 0;
        job.settings.encoding.bitRate = 8'000'000;
        job.settings.encoding.gopSize = 120;
    }
    auto inputs = queuedInputs_;
    if (inputs.empty()) inputs.push_back(source);
    const int targetSelection = batch ? static_cast<int>(SendMessageW(outputSizeCombo_, CB_GETCURSEL, 0, 0)) : 0;
    startQueuedProcessing(std::move(job), std::move(inputs), platformIndex, targetSelection);
    if (!queueSessionActive_.load()) {
        updateView();
        return;
    }
    state_.processing = true;
    state_.status = batch ? L"正在后台检查并加入本地任务队列…" : L"正在加入本地任务队列…";
    state_.outputFrames = 0;
    state_.processingFps = 0;
    updateView();
#else
    state_.status = L"当前构建未启用完整媒体和 ONNX 服务";
    updateView();
#endif
}

void NativeApp::cancelProcessing() {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    if (processingService_) processingService_->cancel();
    stopQueuedProcessing();
    state_.status = L"正在请求取消，等待安全退出…";
    updateView();
#endif
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
bool NativeApp::captureSelectedRestoration(application::ProcessingJob& job) {
    const auto selected = [](HWND combo, const auto& models) -> const restoration::ModelPack* {
        const auto index = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : 0;
        return index > 0 && index <= static_cast<int>(models.size()) ? &models[static_cast<std::size_t>(index - 1)] : nullptr;
    };
    const int strengthIndex = compressionStrengthCombo_ ? static_cast<int>(SendMessageW(compressionStrengthCombo_, CB_GETCURSEL, 0, 0)) : 0;
    if (strengthIndex < 0 || strengthIndex > 3) {
        state_.status = L"压缩修复强度无效，请重新选择开关/强度";
        updateView();
        return false;
    }
    const auto* compression = strengthIndex > 0 ? selected(compressionRestorationCombo_, compressionRestorationModels_) : nullptr;
    if (strengthIndex > 0 && !compression) {
        state_.status = L"请先选择已验证的 STDF CUDA 压缩修复模型";
        updateView();
        return false;
    }
    const bool tiled = compression && compressionTilingCheck_ &&
        SendMessageW(compressionTilingCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return captureRestorationDescriptors(job, selected(temporalRestorationCombo_, temporalRestorationModels_),
        selected(restorationCombo_, spatialRestorationModels_), compression, nativeCompressionStrength(strengthIndex), tiled,
        selected(deblurRestorationCombo_, deblurRestorationModels_));
}

bool NativeApp::captureRestorationDescriptors(application::ProcessingJob& job,
    const restoration::ModelPack* temporal, const restoration::ModelPack* spatial,
    const restoration::ModelPack* compression, float strength, bool tiled, const restoration::ModelPack* deblur) {
    try {
        if (deblur && (!restoration::supportsOnnxCudaOutputRateModel(*deblur) ||
            restoration::outputRateImageTask(deblur->capabilities) != restoration::Task::deblur))
            throw std::invalid_argument("Independent deblur requires a verified same-resolution output-rate model");
        if (deblur && spatial && restoration::outputRateImageTask(spatial->capabilities) != restoration::Task::superResolution)
            throw std::invalid_argument("Independent deblur can only precede a super-resolution output model");
        if (compression && (!restoration::supportsStdfCudaModel(*compression) || !std::isfinite(strength) || strength <= 0.0F || strength > 1.0F))
            throw std::invalid_argument("STDF compression restoration requires a qualified CUDA model and strength in (0, 1]");
        if (!compression && strength != 0.0F)
            throw std::invalid_argument("Compression restoration strength requires a selected STDF model");
        if (tiled && !compression)
            throw std::invalid_argument("Tiled compression restoration requires an explicitly enabled STDF model");
        // Finish every read before changing the captured job or selected packs.
        // A model removed after discovery is an actionable startup error, not
        // an exception that may escape the Win32 command callback.
        const auto temporalHash = temporal ? inference::sha256File(temporal->directory / L"model.json", 256 * 1024) : std::string{};
        const auto spatialHash = spatial ? inference::sha256File(spatial->directory / L"model.json", 256 * 1024) : std::string{};
        const auto deblurHash = deblur ? inference::sha256File(deblur->directory / L"model.json", 256 * 1024) : std::string{};
        const auto compressionHash = compression ? inference::sha256File(compression->directory / L"model.json", 256 * 1024) : std::string{};
        if (temporal) {
            job.temporalRestorationModelDirectory = temporal->directory;
            job.expectedTemporalRestorationDescriptorSha256 = temporalHash;
            temporalRestorationModelPack_ = *temporal;
        }
        if (spatial) {
            job.restorationModelDirectory = spatial->directory;
            job.expectedRestorationDescriptorSha256 = spatialHash;
            restorationModelPack_ = *spatial;
        }
        job.deblurModelDirectory = deblur ? deblur->directory : std::filesystem::path{};
        job.expectedDeblurDescriptorSha256 = deblurHash;
        job.compressionRestorationModelDirectory = compression ? compression->directory : std::filesystem::path{};
        job.expectedCompressionRestorationDescriptorSha256 = compressionHash;
        job.compressionRestorationStrength = strength;
        job.compressionRestorationTiled = tiled;
        return true;
    } catch (const std::exception& error) {
        state_.stage = L"failed";
        state_.status = L"模型文件检查失败：请恢复所选降噪/去模糊/超分模型的 model.json（压缩修复还需已验证的 STDF 描述文件）并确认可读，或重新选择模型后再试。详情：" + wideFromUtf8(error.what());
        updateView();
        return false;
    }
}

void NativeApp::toggleQueuedPause() {
    if (!taskQueue_ || !taskQueue_->busy() || queuePauseTransitionPending_) return;
    queuePauseTransitionPending_ = true;
    if (queuePaused_) {
        taskQueue_->resume();
        state_.stage = L"继续中";
        state_.status = L"正在继续当前任务…";
    } else {
        taskQueue_->pause();
        state_.stage = L"暂停中";
        state_.status = L"正在请求暂停；将在下一安全检查点停住（不发布未完成输出）";
    }
    updateView();
}

void NativeApp::startQueuedProcessing(application::ProcessingJob job, std::vector<std::filesystem::path> inputs, int platformIndex, int outputTargetSelection) {
    if (!taskQueue_) {
        state_.status = L"本地任务队列不可用";
        return;
    }
    if (queueSessionActive_.load()) {
        state_.status = L"已有队列任务正在运行";
        return;
    }
    if (queueOwnerThread_.joinable()) queueOwnerThread_.join();

    const bool batch = inputs.size() > 1 || (inputs.size() == 1 && std::filesystem::is_directory(inputs.front()));
    const auto output = currentOutput();
    std::size_t existingQueued = 0;
    for (const auto& row : taskQueue_->list()) {
        if (row.task.state == TaskState::queued) ++existingQueued;
    }
    if (existingQueued != 0) {
        const auto message = L"本地队列中已有 " + std::to_wstring(existingQueued) +
            L" 个待处理任务。点击“是”后会按队列顺序继续执行这些任务以及本次新任务。\n\n是否继续？";
        if (MessageBoxW(window_, message.c_str(), L"确认运行本地任务队列", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            state_.status = L"已取消启动；原有本地队列保持不变";
            return;
        }
    }
    queueImportStop_ = std::stop_source();
    queueAccepted_ = 0;
    queueRejected_ = 0;
    queueHadFailures_ = false;
    queuePaused_ = false;
    queuePauseTransitionPending_ = false;
    queueFirstRejection_.clear();
    queueRejections_.clear();
    queueSessionActive_.store(true);

    queueOwnerThread_ = std::jthread([this, job = std::move(job), inputs = std::move(inputs), output, batch, platformIndex, outputTargetSelection](std::stop_token ownerStop) mutable {
        std::stop_callback stopBridge(ownerStop, [this] {
            queueImportStop_.request_stop();
            if (taskQueue_) taskQueue_->stop();
        });
        const auto stop = queueImportStop_.get_token();
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        try {
            std::vector<std::string> taskIds;
            if (batch) {
                application::ImportRequest request;
                request.inputs = inputs;
                request.outputDirectory = output;
                request.settings = job;
                request.outputTargetSelection = outputTargetSelection;
                request.container = platformIndex > 0 ? "mp4" : "mkv";
                request.platformBitrate = platformIndex == 0 ? application::PlatformBitrate::unchanged :
                    platformIndex == 1 ? application::PlatformBitrate::youtube : application::PlatformBitrate::localQuality;
                request.recursive = false;
                const auto imported = application::importVideos(*taskQueue_, request, stop,
                    [this](std::size_t current, std::size_t total) {
                        postQueueNotice({L"正在检查批量素材 " + std::to_wstring(current) + L" / " + std::to_wstring(total), 0, 0, false, false, false});
                    });
                taskIds = imported.taskIds;
                accepted = taskIds.size();
                rejected = imported.rejected.size();
                std::wstring detail = L"批量检查完成：已加入 " + std::to_wstring(accepted) + L" 个任务";
                if (rejected != 0) detail += L"，拒绝 " + std::to_wstring(rejected) + L" 个";
                std::wstring firstRejection;
                std::vector<std::wstring> rejectionList;
                rejectionList.reserve(imported.rejected.size());
                for (const auto& item : imported.rejected) {
                    const wchar_t* reason = L"无法读取或处理";
                    switch (item.reason) {
                    case application::ImportRejection::noVideo: reason = L"没有视频轨"; break;
                    case application::ImportRejection::unsupportedColor: reason = L"色彩配置不受支持"; break;
                    case application::ImportRejection::unsupportedModelProfile: reason = L"插帧模型不支持此素材"; break;
                    case application::ImportRejection::unsupportedRestorationModelProfile: reason = L"修复模型不支持此素材"; break;
                    case application::ImportRejection::unsupportedEncoder: reason = L"编码器不可用"; break;
                    case application::ImportRejection::unsupportedOutputDimensions: reason = L"输出尺寸或比例不匹配"; break;
                    case application::ImportRejection::unreadable: break;
                    }
                    if (firstRejection.empty()) firstRejection = item.path.filename().wstring() + L"：" + reason;
                    rejectionList.push_back(item.path.wstring() + L"\r\n  " + reason);
                }
                postQueueNotice({std::move(detail), accepted, rejected, true, false, false, std::move(firstRejection), std::move(rejectionList)});
            } else {
                taskIds = taskQueue_->enqueue({std::move(job)}, stop);
                accepted = taskIds.size();
                postQueueNotice({L"任务已安全写入本地 SQLite 队列", accepted, 0, true, false, false});
            }
            if (stop.stop_requested()) throw std::runtime_error("Queue cancelled");
            if (taskIds.empty()) {
                std::wstring status = L"没有可处理的本地视频";
                if (rejected != 0) status += L"（拒绝 " + std::to_wstring(rejected) + L" 个）";
                postQueueNotice({std::move(status), accepted, rejected, false, true, rejected != 0});
                return;
            }
            if (!taskQueue_->start([this](application::QueueEvent event) {
                postQueueEvent(std::move(event));
            })) throw std::runtime_error("本地任务队列已经在运行");
            taskQueue_->wait();
            postQueueNotice({L"", accepted, rejected, false, true, false});
        } catch (const std::exception& error) {
            if (stop.stop_requested()) {
                postQueueNotice({L"本地队列已取消", accepted, rejected, false, true, false});
            } else {
                postQueueNotice({L"本地队列失败：" + wideFromUtf8(error.what()), accepted, rejected, false, true, true});
            }
        }
    });
}

void NativeApp::stopQueuedProcessing() {
    queueImportStop_.request_stop();
    if (taskQueue_) taskQueue_->stop();
    if (queueOwnerThread_.joinable()) queueOwnerThread_.request_stop();
}

void NativeApp::postQueueEvent(application::QueueEvent event) {
    const HWND callbackWindow = callbackWindow_.load();
    if (!callbackWindow || closing_) return;
    auto* payload = new NativeQueueEvent{std::move(event)};
    if (!PostMessageW(callbackWindow, kQueueEventMessage, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void NativeApp::postQueueNotice(NativeQueueNotice notice) {
    const HWND callbackWindow = callbackWindow_.load();
    if (!callbackWindow || closing_) return;
    auto* payload = new NativeQueueNotice(std::move(notice));
    if (!PostMessageW(callbackWindow, kQueueNoticeMessage, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void NativeApp::handleQueueEvent(application::QueueEvent event) {
    if (event.drained) {
        queuePaused_ = false;
        queuePauseTransitionPending_ = false;
        if (!event.queueError.empty()) {
            state_.stage = L"failed";
            state_.status = L"本地队列失败：" + wideFromUtf8(event.queueError);
        } else if (queueHadFailures_) {
            state_.stage = L"failed";
            state_.status = L"队列已结束，部分素材被拒绝或任务失败";
            if (queueRejected_ != 0) state_.status += L"（拒绝 " + std::to_wstring(queueRejected_) + L" 个）";
            if (!queueFirstRejection_.empty()) state_.status += L" · " + queueFirstRejection_;
        } else if (state_.stage != L"failed" && state_.stage != L"cancelled") {
            state_.stage = L"completed";
            state_.status = L"本地队列已完成";
        }
        state_.processing = false;
        updateView();
        return;
    }
    handleProcessingEvent(std::move(event.processing));
    if (event.processing.kind == application::ProcessingEvent::Kind::completed) {
        queuePaused_ = false;
        queuePauseTransitionPending_ = false;
        state_.processing = true;
        state_.status = L"当前任务完成，继续处理队列…";
    } else if (event.processing.kind == application::ProcessingEvent::Kind::cancelled) {
        queuePaused_ = false;
        queuePauseTransitionPending_ = false;
        state_.processing = true;
        state_.status = L"当前任务已取消，队列正在收尾…";
    } else if (event.processing.kind == application::ProcessingEvent::Kind::failed) {
        queuePaused_ = false;
        queuePauseTransitionPending_ = false;
        queueHadFailures_ = true;
        state_.processing = true;
        state_.status = L"当前任务失败，队列正在收尾…";
    }
    updateView();
}

void NativeApp::handleQueueNotice(NativeQueueNotice notice) {
    if (!notice.rejectionDetail.empty()) queueFirstRejection_ = std::move(notice.rejectionDetail);
    if (!notice.rejectionList.empty()) queueRejections_ = std::move(notice.rejectionList);
    if (notice.accepted != 0 || notice.rejected != 0) {
        queueAccepted_ = notice.accepted;
        queueRejected_ = notice.rejected;
        if (notice.rejected != 0) queueHadFailures_ = true;
    }
    if (!notice.status.empty()) state_.status = std::move(notice.status);
    if (notice.importFinished) {
        state_.stage = L"importing";
        state_.processing = true;
    }
    if (notice.failed) state_.stage = L"failed";
    if (notice.ownerFinished) {
        queueSessionActive_.store(false);
        queuePaused_ = false;
        queuePauseTransitionPending_ = false;
        state_.processing = false;
        if (notice.failed) state_.stage = L"failed";
        if (notice.failed && !queueFirstRejection_.empty()) state_.status += L" · " + queueFirstRejection_;
        if (queueOwnerThread_.joinable()) queueOwnerThread_.join();
        if (!notice.failed && queueImportStop_.stop_requested()) {
            state_.stage = L"cancelled";
            state_.status = L"队列已停止";
            try {
                const auto tasks = taskQueue_->list();
                const auto remaining = std::count_if(tasks.begin(), tasks.end(), [](const auto& item) {
                    return item.task.state == TaskState::queued;
                });
                if (remaining != 0) state_.status += L"；" + std::to_wstring(remaining) + L" 个任务仍在队列，下次运行前会确认";
            } catch (const std::exception&) {
                state_.status += L"；无法读取剩余任务状态";
            }
        }
        if (!closing_ && historyWindow_ && IsWindow(historyWindow_)) refreshTaskHistory();
    }
    updateView();
}
#endif

void NativeApp::closeWindow() {
    closing_ = true;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    stopQueuedProcessing();
    if (processingService_ && processingService_->busy()) processingService_->cancel();
#endif
    DestroyWindow(window_);
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
void NativeApp::discoverLocalResources() {
    refreshLocalModels();
    for (const auto& runtime : {applicationDirectory_ / L"runtime", applicationDirectory_ / L"onnxruntime"}) {
        if (refreshRuntime(runtime)) break;
    }
}

NativeApp::LocalModel NativeApp::inspectLocalModel(const std::filesystem::path& directory) const {
    LocalModel item;
    item.directory = std::filesystem::absolute(directory).lexically_normal();
    item.name = item.directory.filename().wstring();
    try {
        const auto checked = checkedNativeModelDirectory(item.directory);
        std::string rifeFailure;
        try {
            const auto pack = inference::loadModelPack(checked);
            if (pack.family != "rife") throw std::runtime_error("Not a RIFE model pack");
            item.kind = 0;
            item.name = wideFromUtf8(pack.displayName.empty() ? pack.id : pack.displayName);
            item.version = wideFromUtf8(pack.version);
            item.modelSha256 = wideFromUtf8(pack.sha256);
            item.licenseId = wideFromUtf8(pack.licenseId);
            item.sourceRepository = wideFromUtf8(pack.sourceRepository);
            item.upstreamCommit = wideFromUtf8(pack.upstreamCommit);
            item.licenseScopeEvidence = wideFromUtf8(pack.licenseScopeEvidence);
            item.numericalProfile = wideFromUtf8(pack.numericalProfile);
            for (const auto& backend : pack.validatedBackends) {
                if (!item.validatedBackends.empty()) item.validatedBackends += L"、";
                item.validatedBackends += wideFromUtf8(backend);
            }
            item.capabilities = L"RIFE 帧间插帧；适配器 " + wideFromUtf8(pack.adapterId) + L" / " + std::to_wstring(pack.adapterVersion);
            for (const auto& profile : pack.supportedInputProfiles) {
                if (!item.profiles.empty()) item.profiles += L"、";
                item.profiles += wideFromUtf8(profile);
            }
            item.profiles += pack.hdrQualified ? L"；包声明支持 HDR" : L"；未声明 HDR 能力";
            item.verified = pack.metadataIntegrityVerified;
            item.usable = item.verified && pack.adapterId == "rife-sdr-v1" && pack.adapterVersion == 1;
            item.verification = item.verified ? L"权重、许可、来源清单与验证报告的哈希一致"
                : L"仅权重哈希通过；旧格式缺少完整元数据校验，不能设为默认";
            if (item.verified && !item.usable) item.verification += L"；当前客户端不支持此适配器";
            return item;
        } catch (const std::exception& error) {
            rifeFailure = error.what();
        }
        try {
            const auto pack = restoration::loadModelPack(checked);
            const auto& capabilities = pack.capabilities;
            item.name = wideFromUtf8(pack.displayName.empty() ? pack.id : pack.displayName);
            item.version = wideFromUtf8(pack.version);
            item.modelSha256 = wideFromUtf8(pack.graphSha256);
            item.licenseId = wideFromUtf8(pack.licenseId);
            item.sourceRepository = wideFromUtf8(pack.sourceRepository);
            item.upstreamCommit = wideFromUtf8(pack.upstreamCommit);
            item.licenseScopeEvidence = wideFromUtf8(pack.licenseScopeEvidence);
            item.numericalProfile = wideFromUtf8(pack.numericalProfile);
            for (const auto& backend : pack.validatedBackends) {
                if (!item.validatedBackends.empty()) item.validatedBackends += L"、";
                item.validatedBackends += wideFromUtf8(backend);
            }
            const bool sr = std::find(capabilities.tasks.begin(), capabilities.tasks.end(), restoration::Task::superResolution) != capabilities.tasks.end();
            const bool temporal = std::find(capabilities.placements.begin(), capabilities.placements.end(), restoration::Placement::sourceRateBeforeInterpolation) != capabilities.placements.end();
            const bool compression = restoration::supportsStdfCudaModel(pack);
            const bool deblur = restoration::outputRateImageTask(capabilities) == restoration::Task::deblur;
            item.kind = compression ? 3 : temporal ? 2 : deblur ? 4 : sr ? 1 : -1;
            for (const auto task : capabilities.tasks) {
                if (!item.capabilities.empty()) item.capabilities += L"、";
                switch (task) {
                case restoration::Task::denoise: item.capabilities += L"降噪"; break;
                case restoration::Task::deblur: item.capabilities += L"去模糊"; break;
                case restoration::Task::deblock: item.capabilities += L"去块效应"; break;
                case restoration::Task::superResolution: item.capabilities += L"超分辨率"; break;
                }
            }
            item.capabilities += L"；比例 " + std::to_wstring(capabilities.scaleNumerator) + L"/" + std::to_wstring(capabilities.scaleDenominator);
            item.capabilities += L"；时域前/后 " + std::to_wstring(capabilities.temporalFramesBefore) + L"/" + std::to_wstring(capabilities.temporalFramesAfter);
            item.capabilities += L"；适配器 " + wideFromUtf8(capabilities.adapterId);
            for (const auto& profile : capabilities.supportedInputProfiles) {
                if (!item.profiles.empty()) item.profiles += L"、";
                item.profiles += wideFromUtf8(profile);
            }
            item.verified = pack.metadataIntegrityVerified;
            item.usable = item.verified && (compression || (item.kind == 2 && restoration::supportsOnnxCudaTemporalModel(pack)) ||
                (item.kind == 1 && capabilities.scaleNumerator == 2 && capabilities.scaleDenominator == 1 && restoration::supportsOnnxCudaOutputRateModel(pack)) ||
                (item.kind == 4 && capabilities.scaleNumerator == 1 && capabilities.scaleDenominator == 1 && restoration::supportsOnnxCudaOutputRateModel(pack)));
            item.verification = L"权重、许可、来源清单与验证报告的哈希一致";
            if (compression) item.verification += L"；STDF 仅限本机 CUDA / BT.709 SDR8；整帧 ≤384 × 256，高清分块需独立开启，默认关闭";
            if (deblur) item.verification += L"；实验性 NAFNet 同尺寸去模糊；仅已验证 SDR8，可能过锐或伪影；增加推理与显存开销；自动细节与自然动态曝光合成互斥；默认关闭";
            if (!item.usable) item.verification += L"；当前客户端不支持该能力/比例，不能设为默认";
            return item;
        } catch (const std::exception& error) {
            // Keep both diagnostics: the directory may contain either pack family.
            item.verification = L"校验失败；RIFE：" + wideFromUtf8(rifeFailure) + L"；修复模型：" + wideFromUtf8(error.what());
        }
    } catch (const std::exception& error) {
        item.verification = L"目录不可用或不是安全的本地目录：" + wideFromUtf8(error.what());
    }
    return item;
}

void NativeApp::refreshLocalModels() {
    if (!modelChangesAllowed()) return;
    std::vector<std::filesystem::path> candidates;
    const auto add = [&](const std::filesystem::path& path) {
        if (!path.empty() && !modelPreferences_.isExcluded(path) && !NativeModelPreferences::contains(candidates, path))
            candidates.push_back(std::filesystem::absolute(path).lexically_normal());
    };
    const auto models = applicationDirectory_ / L"models";
    std::filesystem::path markerSelection;
    try {
        (void)checkedNativeModelDirectory(models, false);
        const DWORD descriptor = GetFileAttributesW((models / L"model.json").c_str());
        if (descriptor != INVALID_FILE_ATTRIBUTES) add(models);
        for (const auto& entry : std::filesystem::directory_iterator(models)) {
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) add(entry.path());
        }
        const auto marker = models / L"default-model.txt";
        const DWORD attributes = GetFileAttributesW(marker.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            std::ifstream input(marker, std::ios::binary);
            std::string line;
            if (std::getline(input, line) && line.size() < 32768) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
                markerSelection = std::filesystem::path(wideFromUtf8(line));
                if (!markerSelection.empty() && markerSelection.is_relative())
                    markerSelection = markerSelection.has_parent_path() ? applicationDirectory_ / markerSelection : models / markerSelection;
                if (!markerSelection.empty()) add(markerSelection);
            }
        }
    } catch (const std::exception&) {
        // A missing bundled models directory is normal for a fresh install.
    }
    for (const auto& path : modelPreferences_.registered) add(path);
    for (const auto& path : modelPreferences_.defaults) add(path);
    localModels_.clear();
    for (const auto& path : candidates) localModels_.push_back(inspectLocalModel(path));
    std::sort(localModels_.begin(), localModels_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name == rhs.name ? lhs.directory.wstring() < rhs.directory.wstring() : lhs.name < rhs.name;
    });
    auto selected = modelPreferences_.defaults[0];
    if (selected.empty()) {
        for (const auto& preferred : {modelDirectory_, markerSelection}) {
            const auto found = std::find_if(localModels_.begin(), localModels_.end(), [&](const auto& item) {
                return item.kind == 0 && item.usable && !preferred.empty() && NativeModelPreferences::key(item.directory) == NativeModelPreferences::key(preferred);
            });
            if (found != localModels_.end()) { selected = found->directory; break; }
        }
        if (selected.empty()) {
            const auto found = std::find_if(localModels_.begin(), localModels_.end(), [](const auto& item) { return item.kind == 0 && item.usable; });
            if (found != localModels_.end()) selected = found->directory;
        }
    }
    modelPack_.reset();
    modelDirectory_.clear();
    state_.modelReady = false;
    modelSummary_ = selected.empty() ? L"尚未登记可用的 RIFE 模型" : L"默认 RIFE 模型校验失败；请打开本地模型中心";
    const auto found = std::find_if(localModels_.begin(), localModels_.end(), [&](const auto& item) {
        return item.kind == 0 && item.usable && !selected.empty() && NativeModelPreferences::key(item.directory) == NativeModelPreferences::key(selected);
    });
    if (found != localModels_.end()) (void)refreshModel(found->directory);
    discoverRestorationModels();
    syncRestorationSelectors();
    updateModelCenter();
    if (window_) updateView();
}

bool NativeApp::modelChangesAllowed() const {
    return !queueSessionActive_.load() && (!processingService_ || !processingService_->busy());
}

bool NativeApp::saveModelPreferences(const NativeModelPreferences& preferences) {
    if (!modelPreferencesWritable_ || !modelChangesAllowed()) {
        if (!modelChangesAllowed()) modelCenterNotice_ = L"视频检查或任务运行期间，暂不能更改模型。";
        updateModelDetails();
        return false;
    }
    try {
        if (!dataDirectory_.writable || !dataDirectory_.preferencesReady)
            throw std::runtime_error("Native model data directory is unavailable");
        (void)checkedNativeModelDirectory(dataDirectory_.directory, false);
        saveNativeModelPreferences(dataDirectory_.directory / L"local-models.txt", preferences);
        modelPreferences_ = preferences;
        return true;
    } catch (const std::exception&) {
        modelCenterNotice_ = L"模型设置保存失败；更改未生效。请检查详情中用户数据目录的写入权限。";
        updateModelDetails();
        return false;
    }
}

void NativeApp::startModelPackageInstall(const std::filesystem::path& archive, std::vector<std::wstring> mirrorUrls) {
    if (!modelPreferencesWritable_ || !dataDirectory_.writable || !dataDirectory_.preferencesReady ||
        !modelChangesAllowed() || modelInstallActive_.load()) return;
    if (modelInstallThread_.joinable()) modelInstallThread_.join();
    auto manifestPath = archive;
    if (!mirrorUrls.empty()) manifestPath.clear();
    else manifestPath.replace_extension(L".distribution.json");
    {
        std::lock_guard lock(modelInstallMutex_);
        modelInstallState_ = {};
        modelInstallState_.status = L"准备校验模型包…";
    }
    modelInstallActive_.store(true);
    const bool downloading = !mirrorUrls.empty();
    try {
        modelInstallThread_ = std::jthread([this, archive, manifestPath, mirrorUrls = std::move(mirrorUrls)](std::stop_token stopToken) {
            std::optional<std::filesystem::path> installed;
            std::wstring failure;
            std::wstring status;
            try {
                if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
                auto resolvedArchive = archive;
                auto resolvedManifestPath = manifestPath;
                std::vector<std::filesystem::path> downloadedManifests;
                if (!mirrorUrls.empty()) {
                    const auto downloadsRoot = dataDirectory_.directory / L"ModelDownloads";
                    prepareWritableNativeDataDirectory(downloadsRoot);
                    (void)checkedNativeModelDirectory(downloadsRoot, false);
                    std::wstring mirrorFailures;
                    for (std::size_t mirrorIndex = 0; mirrorIndex < mirrorUrls.size(); ++mirrorIndex) {
                        const auto& mirrorUrl = mirrorUrls[mirrorIndex];
                        try {
                            if (!mirrorUrl.starts_with(L"https://") || mirrorUrl.find_first_of(L"?#") != std::wstring::npos)
                                throw std::invalid_argument("Use a lowercase HTTPS ZIP URL without a query or fragment");
                            auto baseUrl = mirrorUrl;
                            auto urlEnd = baseUrl.size();
                            while (urlEnd > 0 && (baseUrl[urlEnd - 1] == L'/' || baseUrl[urlEnd - 1] == L'\\')) --urlEnd;
                            baseUrl.resize(urlEnd);
                            const auto slash = baseUrl.find_last_of(L'/');
                            const auto dot = baseUrl.find_last_of(L'.');
                            if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ||
                                baseUrl.substr(dot) != L".zip")
                                throw std::invalid_argument("Each mirror URL must end in .zip");
                            const auto manifestUrl = baseUrl.substr(0, dot) + L".distribution.json";
                            const auto manifestCandidate = downloadsRoot / (L"mirror-" + std::to_wstring(mirrorIndex) + L".distribution.json");
                            {
                                std::lock_guard lock(modelInstallMutex_);
                                modelInstallState_.completedBytes = 0;
                                modelInstallState_.totalBytes = 0;
                                modelInstallState_.status = L"正在获取镜像 " + std::to_wstring(mirrorIndex + 1) + L" 的模型清单";
                            }
                            const auto reportProgress = [this, mirrorIndex](NativeModelDownloadProgress value) {
                                std::lock_guard lock(modelInstallMutex_);
                                modelInstallState_.completedBytes = value.completedBytes;
                                modelInstallState_.totalBytes = value.totalBytes;
                                modelInstallState_.status = L"正在下载镜像 " + std::to_wstring(mirrorIndex + 1) + L" 的 ZIP 或校验清单";
                            };
                            downloadNativeModelFile(manifestUrl, manifestCandidate, 0, {}, 1024 * 1024,
                                stopToken, reportProgress);
                            downloadedManifests.push_back(manifestCandidate);
                            const auto manifestBytes = inference::readLocalFile(manifestCandidate, 1024 * 1024);
                            const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
                            const auto mirrorManifest = nlohmann::json::parse(manifestText);
                            const auto& artifact = mirrorManifest.at("artifact");
                            const auto fileName = artifact.at("fileName").get<std::string>();
                            const auto archiveSize = artifact.at("sizeBytes").get<std::uint64_t>();
                            const auto archiveSha = artifact.at("sha256").get<std::string>();
                            const auto safeArchiveName = [](const std::string& value) {
                                if (value.empty() || value.size() > 128 || value == "." || value == ".." ||
                                    !((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= 'A' && value.front() <= 'Z') ||
                                      (value.front() >= '0' && value.front() <= '9')) || value.back() == '.') return false;
                                const auto extension = value.find('.');
                                auto deviceName = value.substr(0, extension);
                                std::transform(deviceName.begin(), deviceName.end(), deviceName.begin(), [](unsigned char ch) {
                                    return static_cast<char>(std::tolower(ch));
                                });
                                if (deviceName == "con" || deviceName == "prn" || deviceName == "aux" || deviceName == "nul" ||
                                    deviceName == "com1" || deviceName == "com2" || deviceName == "com3" || deviceName == "com4" ||
                                    deviceName == "com5" || deviceName == "com6" || deviceName == "com7" || deviceName == "com8" || deviceName == "com9" ||
                                    deviceName == "lpt1" || deviceName == "lpt2" || deviceName == "lpt3" || deviceName == "lpt4" ||
                                    deviceName == "lpt5" || deviceName == "lpt6" || deviceName == "lpt7" || deviceName == "lpt8" || deviceName == "lpt9") return false;
                                return std::all_of(value.begin(), value.end(), [](char ch) {
                                    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
                                });
                            };
                            const auto validSha = [](const std::string& value) {
                                return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
                                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                                });
                            };
                            if (!safeArchiveName(fileName) || archiveSize == 0 || archiveSize > 1024ULL * 1024 * 1024 || !validSha(archiveSha))
                                throw std::invalid_argument("Mirror manifest contains an unsafe file name or invalid package size/hash");
                            const auto candidateArchive = downloadsRoot / std::filesystem::path(wideFromUtf8(fileName));
                            {
                                std::lock_guard lock(modelInstallMutex_);
                                modelInstallState_.completedBytes = 0;
                                modelInstallState_.totalBytes = archiveSize;
                                modelInstallState_.status = L"正在下载并校验镜像 " + std::to_wstring(mirrorIndex + 1);
                            }
                            downloadNativeModelFile(mirrorUrl, candidateArchive, archiveSize, archiveSha,
                                1024ULL * 1024 * 1024, stopToken, reportProgress);
                            resolvedArchive = candidateArchive;
                            resolvedManifestPath = manifestCandidate;
                            break;
                        } catch (const NativePackageInstallCancelled&) {
                            throw;
                        } catch (const std::exception& error) {
                            if (!mirrorFailures.empty()) mirrorFailures += L"；";
                            mirrorFailures += L"镜像 " + std::to_wstring(mirrorIndex + 1) + L"：" + wideFromUtf8(error.what());
                        }
                    }
                    if (resolvedArchive.empty() || resolvedManifestPath.empty())
                        throw std::runtime_error("All model mirrors failed: " + nativeModelUtf8(mirrorFailures));
                }
                if (resolvedArchive.empty()) throw std::invalid_argument("No model package was selected");
                (void)checkedNativeModelDirectory(resolvedManifestPath.parent_path(), false);
                const DWORD manifestAttributes = GetFileAttributesW(resolvedManifestPath.c_str());
                if (manifestAttributes == INVALID_FILE_ATTRIBUTES || (manifestAttributes &
                    (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                    throw std::runtime_error("Distribution manifest must be a regular local file");
                const auto manifestBytes = inference::readLocalFile(resolvedManifestPath, 1024 * 1024);
                const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
                const auto manifest = nlohmann::json::parse(manifestText);
                if (!manifest.is_object() || manifest.at("schemaVersion") != 1)
                    throw std::invalid_argument("Unsupported model distribution manifest version");

                const auto boundedString = [](const nlohmann::json& object, const char* name, std::size_t limit) {
                    const auto& value = object.at(name);
                    if (!value.is_string()) throw std::invalid_argument(std::string("Invalid model distribution field: ") + name);
                    const auto text = value.get<std::string>();
                    if (text.empty() || text.size() > limit || text.find('\0') != std::string::npos)
                        throw std::invalid_argument(std::string("Invalid model distribution field: ") + name);
                    return text;
                };
                const auto safeComponent = [](const std::string& value) {
                    if (value.empty() || value.size() > 128 ||
                        !((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= 'A' && value.front() <= 'Z') ||
                          (value.front() >= '0' && value.front() <= '9'))) return false;
                    return value != "." && value != ".." && std::all_of(value.begin(), value.end(), [](char ch) {
                        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
                    });
                };
                const auto validHash = [](const std::string& value) {
                    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
                        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                    });
                };

                const auto packageId = boundedString(manifest, "packageId", 128);
                if (!safeComponent(packageId)) throw std::invalid_argument("Invalid model package identity");
                const auto& modelMetadata = manifest.at("model");
                const auto modelId = boundedString(modelMetadata, "id", 128);
                const auto modelVersion = boundedString(modelMetadata, "version", 128);
                const auto modelFamily = boundedString(modelMetadata, "family", 64);
                const auto licenseId = boundedString(modelMetadata, "license", 128);
                const auto modelSha256 = boundedString(modelMetadata, "modelSha256", 64);
                if (modelFamily != "rife" || !safeComponent(modelId) || !safeComponent(modelVersion) || !validHash(modelSha256))
                    throw std::invalid_argument("This Model Center installer accepts only a declared RIFE model pack");

                const auto& artifact = manifest.at("artifact");
                const auto archiveName = boundedString(artifact, "fileName", 256);
                if (archiveName != nativeModelUtf8(resolvedArchive.filename().wstring()))
                    throw std::invalid_argument("The selected ZIP file name does not match its distribution manifest");
                NativePackageArchivePlan plan;
                plan.rootDirectory = boundedString(artifact, "rootDirectory", 128);
                plan.archiveSizeBytes = artifact.at("sizeBytes").get<std::uint64_t>();
                plan.archiveSha256 = boundedString(artifact, "sha256", 64);
                if (!validHash(plan.archiveSha256)) throw std::invalid_argument("Invalid model archive SHA-256");
                plan.maximumUncompressedBytes = 512ULL * 1024 * 1024;

                const std::array<std::string, 5> requiredFiles{
                    "LICENSE.txt", "model.json", "model.onnx", "source-manifest.json", "validation.json"};
                const auto& fileEntries = manifest.at("files");
                if (!fileEntries.is_array() || fileEntries.size() != requiredFiles.size())
                    throw std::invalid_argument("The model manifest does not list the exact files required by the RIFE pack");
                std::set<std::string> seenFiles;
                for (const auto& entry : fileEntries) {
                    const auto path = boundedString(entry, "path", 128);
                    if (std::find(requiredFiles.begin(), requiredFiles.end(), path) == requiredFiles.end() || !seenFiles.insert(path).second)
                        throw std::invalid_argument("The model manifest contains an unrecognized or duplicate file");
                    const auto size = entry.at("sizeBytes").get<std::uint64_t>();
                    const auto digest = boundedString(entry, "sha256", 64);
                    if (size == 0 || size > plan.maximumUncompressedBytes || !validHash(digest))
                        throw std::invalid_argument("The model manifest contains an invalid file size or SHA-256");
                    plan.files.push_back({path, size, digest});
                }
                if (seenFiles.size() != requiredFiles.size())
                    throw std::invalid_argument("The model manifest is missing a required RIFE pack file");

                auto modelsRoot = dataDirectory_.directory / L"Models";
                prepareWritableNativeDataDirectory(modelsRoot);
                (void)checkedNativeModelDirectory(modelsRoot, false);
                const auto stage = uniqueNativeDataPath(modelsRoot, L".model-install-");
                const auto progress = [this](NativePackageArchiveProgress value) {
                    std::lock_guard lock(modelInstallMutex_);
                    modelInstallState_.completedBytes = value.completedBytes;
                    modelInstallState_.totalBytes = value.totalBytes;
                    modelInstallState_.status = value.phase == "archive" ? L"正在校验归档 SHA-256" :
                        value.phase == "extract" ? L"正在安全解压到暂存目录" : L"正在校验模型文件 SHA-256";
                };
                const auto modelDirectory = extractNativeModelPackageArchive(resolvedArchive, plan, stage, stopToken, progress);
                struct StageCleanup final {
                    std::filesystem::path path;
                    ~StageCleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
                } cleanup{stage};
                if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
                {
                    std::lock_guard lock(modelInstallMutex_);
                    modelInstallState_.completedBytes = 0;
                    modelInstallState_.totalBytes = 0;
                    modelInstallState_.status = L"正在校验 RIFE 模型元数据与 schema-v2 契约";
                }
                const auto verified = inference::loadModelPack(modelDirectory);
                if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
                if (verified.schemaVersion != 2 || !verified.metadataIntegrityVerified || verified.family != modelFamily ||
                    verified.id != modelId || verified.version != modelVersion || verified.sha256 != modelSha256 ||
                    verified.licenseId != licenseId)
                    throw std::runtime_error("The extracted RIFE pack does not match its manifest identity and schema-v2 metadata");

                const auto managedName = modelId.substr(0, 48) + "-" + modelVersion.substr(0, 32) + "-" + verified.sha256.substr(0, 12);
                if (!safeComponent(managedName)) throw std::invalid_argument("The verified model identity cannot form a safe managed directory");
                const auto destination = modelsRoot / std::filesystem::path(managedName);
                const DWORD existing = GetFileAttributesW(destination.c_str());
                if (existing != INVALID_FILE_ATTRIBUTES) {
                    (void)checkedNativeModelDirectory(destination, false);
                    {
                        std::lock_guard lock(modelInstallMutex_);
                        modelInstallState_.status = L"正在重新校验托管目录中的已安装版本";
                    }
                    const auto installedPack = inference::loadModelPack(destination);
                    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
                    if (installedPack.sha256 != verified.sha256 || installedPack.id != verified.id || installedPack.version != verified.version)
                        throw std::runtime_error("A different model already occupies the managed version directory");
                    installed = destination;
                    status = mirrorUrls.empty() ? L"此模型版本已安装；已重新校验现有文件。" : L"镜像下载包已校验；此模型版本已安装。";
                } else {
                    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
                    if (!MoveFileExW(modelDirectory.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("Cannot atomically publish the verified model into the managed directory");
                    installed = destination;
                    status = mirrorUrls.empty() ? L"模型包已验证并原子安装。" : L"模型包已下载、校验并原子安装。";
                }
                if (!mirrorUrls.empty()) {
                    std::error_code cleanupError;
                    std::filesystem::remove(resolvedArchive, cleanupError);
                    for (const auto& downloadedManifest : downloadedManifests)
                        std::filesystem::remove(downloadedManifest, cleanupError);
                }
            } catch (const NativePackageInstallCancelled&) {
                failure = L"安装已取消；暂存文件已清理，现有模型未更改。";
            } catch (const std::exception& error) {
                failure = wideFromUtf8(error.what());
            } catch (...) {
                failure = L"模型包安装失败；现有模型未更改。";
            }
            {
                std::lock_guard lock(modelInstallMutex_);
                modelInstallState_.installedDirectory = installed;
                modelInstallState_.failure = failure;
                modelInstallState_.status = status;
                modelInstallState_.finished = true;
            }
            modelInstallActive_.store(false);
        });
        if (!modelCenterWindow_ || SetTimer(modelCenterWindow_, kModelInstallTimer, 120, nullptr) == 0) {
            modelInstallThread_.request_stop();
            if (modelInstallThread_.joinable()) modelInstallThread_.join();
            modelInstallActive_.store(false);
            throw std::runtime_error("Cannot start the model installation progress timer");
        }
        modelCenterNotice_ = !downloading
            ? L"正在后台校验模型包；取消不会影响已安装模型。"
            : L"正在后台下载并验证模型包；中断后可从同一镜像续传。";
        updateModelDetails();
    } catch (const std::exception& error) {
        modelInstallActive_.store(false);
        modelCenterNotice_ = L"无法启动模型安装：" + wideFromUtf8(error.what());
        updateModelDetails();
    }
}

void NativeApp::cancelModelPackageInstall() {
    if (modelInstallThread_.joinable()) modelInstallThread_.request_stop();
    if (modelInstallActive_.load()) {
        std::lock_guard lock(modelInstallMutex_);
        modelInstallState_.status = L"正在取消；解压暂存会清理，未完成的下载会保留以便续传…";
    }
    if (modelCenterWindow_) updateModelDetails();
}

void NativeApp::pollModelPackageInstall() {
    if (!modelCenterWindow_) return;
    if (modelInstallActive_.load()) { updateModelDetails(); return; }
    KillTimer(modelCenterWindow_, kModelInstallTimer);
    if (modelInstallThread_.joinable()) modelInstallThread_.join();
    std::optional<std::filesystem::path> installed;
    std::wstring failure;
    std::wstring status;
    {
        std::lock_guard lock(modelInstallMutex_);
        installed = modelInstallState_.installedDirectory;
        failure = modelInstallState_.failure;
        status = modelInstallState_.status;
    }
    if (installed) {
        modelCenterSelectedPath_ = *installed;
        if (modelChangesAllowed() && registerLocalModel(*installed)) {
            modelCenterNotice_ = status + L" 已登记到模型中心。";
        } else {
            modelCenterNotice_ = status + L" 当前任务结束后，可在此登记该目录：\r\n" + installed->wstring();
        }
    } else if (!failure.empty()) modelCenterNotice_ = L"模型安装未完成：" + failure;
    updateModelCenter();
    if (closeModelCenterWhenInstallEnds_ && modelCenterWindow_) {
        closeModelCenterWhenInstallEnds_ = false;
        DestroyWindow(modelCenterWindow_);
    }
}

bool NativeApp::registerLocalModel(const std::filesystem::path& directory) {
    if (!modelChangesAllowed()) return false;
    const auto item = inspectLocalModel(directory);
    if (!item.verified) {
        modelCenterNotice_ = L"无法登记：" + item.verification;
        updateModelDetails();
        return false;
    }
    auto preferences = modelPreferences_;
    preferences.registerDirectory(item.directory);
    if (!saveModelPreferences(preferences)) return false;
    modelCenterNotice_ = item.usable ? L"已登记本地目录；请选择模型并设为默认。未复制任何模型文件。"
        : L"已登记并完成完整性校验，但当前客户端不支持该能力；不能设为默认。";
    refreshLocalModels();
    return true;
}

bool NativeApp::setDefaultLocalModel(std::size_t index) {
    if (index >= localModels_.size() || !modelChangesAllowed()) return false;
    const auto item = inspectLocalModel(localModels_[index].directory);
    if (!item.usable || item.kind < 0) {
        modelCenterNotice_ = L"不能设为默认：模型已变化、校验未通过或当前能力不受支持。";
        refreshLocalModels();
        return false;
    }
    auto preferences = modelPreferences_;
    preferences.registerDirectory(item.directory);
    preferences.defaults[static_cast<std::size_t>(item.kind)] = item.directory;
    if (!saveModelPreferences(preferences)) return false;
    modelCenterNotice_ = L"默认模型已保存，下次启动继续使用；可选修复已同步到主窗口。";
    refreshLocalModels();
    return true;
}

bool NativeApp::unregisterLocalModel(std::size_t index) {
    if (index >= localModels_.size() || !modelChangesAllowed()) return false;
    auto preferences = modelPreferences_;
    preferences.unregisterDirectory(localModels_[index].directory);
    if (!saveModelPreferences(preferences)) return false;
    modelCenterNotice_ = L"已取消应用登记并清除对应默认项；原目录与全部文件均保留。";
    refreshLocalModels();
    return true;
}

bool NativeApp::refreshModel(const std::filesystem::path& directory) {
    try {
        const auto pack = inference::loadModelPack(directory);
        if (pack.family != "rife") return false;
        modelPack_ = pack;
        modelDirectory_ = directory;
        modelSummary_ = wideFromUtf8(pack.displayName.empty() ? pack.id : pack.displayName) + L"  " + wideFromUtf8(pack.version);
        state_.modelReady = true;
        return true;
    } catch (const std::exception& error) {
        modelPack_.reset();
        state_.modelReady = false;
        modelSummary_ = L"模型不可用：" + wideFromUtf8(error.what());
        return false;
    }
}

bool NativeApp::refreshRuntime(const std::filesystem::path& directory) {
    static constexpr std::array<const wchar_t*, 12> required{
        L"onnxruntime.dll", L"onnxruntime_providers_cuda.dll", L"onnxruntime_providers_shared.dll",
        L"cudart64_12.dll", L"cublas64_12.dll", L"cublasLt64_12.dll", L"cufft64_11.dll",
        L"cudnn_graph64_9.dll", L"cudnn_ops64_9.dll", L"cudnn_cnn64_9.dll",
        L"cudnn_adv64_9.dll", L"cudnn64_9.dll"};
    std::vector<std::wstring> missing;
    for (const auto* name : required) if (!std::filesystem::is_regular_file(directory / name)) missing.emplace_back(name);
    runtimeDirectory_ = directory;
    state_.runtimeReady = missing.empty();
    if (state_.runtimeReady) runtimeSummary_ = L"运行库已就绪 · CUDA";
    else runtimeSummary_ = L"运行库待检查 · 请在专业设置中选择目录";
    return state_.runtimeReady;
}

void NativeApp::discoverRestorationModels() {
    temporalRestorationModels_.clear();
    spatialRestorationModels_.clear();
    deblurRestorationModels_.clear();
    compressionRestorationModels_.clear();
    for (const auto& item : localModels_) {
        if (!item.usable || item.kind == 0) continue;
        try {
            const auto pack = restoration::loadModelPack(checkedNativeModelDirectory(item.directory));
            if (item.kind == 2) temporalRestorationModels_.push_back(pack);
            else if (item.kind == 1) spatialRestorationModels_.push_back(pack);
            else if (item.kind == 4 && restoration::supportsOnnxCudaOutputRateModel(pack)) deblurRestorationModels_.push_back(pack);
            else if (item.kind == 3 && restoration::supportsStdfCudaModel(pack)) compressionRestorationModels_.push_back(pack);
        } catch (const std::exception&) {
            // A pack changed during the refresh; it cannot enter a job selector.
        }
    }
    std::sort(temporalRestorationModels_.begin(), temporalRestorationModels_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.displayName < rhs.displayName;
    });
    std::sort(spatialRestorationModels_.begin(), spatialRestorationModels_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.displayName < rhs.displayName;
    });
    std::sort(deblurRestorationModels_.begin(), deblurRestorationModels_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.displayName < rhs.displayName;
    });
    std::sort(compressionRestorationModels_.begin(), compressionRestorationModels_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.displayName < rhs.displayName;
    });
}

void NativeApp::syncRestorationSelectors() {
    const auto populate = [&](HWND combo, const auto& models, int kind, const wchar_t* off) {
        if (!combo) return;
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(off));
        int selection = 0;
        for (std::size_t index = 0; index < models.size(); ++index) {
            const auto name = restorationSelectorLabel(models[index]);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            const auto& selected = modelPreferences_.defaults[static_cast<std::size_t>(kind)];
            if (!selected.empty() && NativeModelPreferences::key(models[index].directory) == NativeModelPreferences::key(selected))
                selection = static_cast<int>(index + 1);
        }
        SendMessageW(combo, CB_SETCURSEL, selection, 0);
    };
    populate(temporalRestorationCombo_, temporalRestorationModels_, 2, L"关闭（初始默认：不使用可选降噪）");
    populate(deblurRestorationCombo_, deblurRestorationModels_, 4, L"关闭（默认：不使用实验性去模糊）");
    if (deblurRestorationCombo_) SendMessageW(deblurRestorationCombo_, CB_SETCURSEL, 0, 0);
    automaticDetailRestoration_ = false;
    if (detailAdjustmentEdit_ && controlText(detailAdjustmentEdit_) != L"0") SetWindowTextW(detailAdjustmentEdit_, L"0");
    populate(restorationCombo_, spatialRestorationModels_, 1, L"关闭（初始默认：不使用超分辨率）");
    populate(compressionRestorationCombo_, compressionRestorationModels_, 3, L"未选择压缩修复模型");
    // Refresh/default-model changes cannot carry an experimental strength over
    // to another graph. The user must explicitly opt in again.
    if (compressionStrengthCombo_) SendMessageW(compressionStrengthCombo_, CB_SETCURSEL, 0, 0);
    if (compressionTilingCheck_) SendMessageW(compressionTilingCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
}

void NativeApp::updateRestorationControls() {
    if (!temporalRestorationCombo_ || !deblurRestorationCombo_ || !restorationCombo_ || !outputSizeCombo_) return;
    if (!automaticDenoiseAvailable()) {
        automaticTemporalDenoise_ = false;
        if (denoiseAdjustmentEdit_ && controlText(denoiseAdjustmentEdit_) != L"0") SetWindowTextW(denoiseAdjustmentEdit_, L"0");
    }
    state_.automaticTemporalDenoise = automaticTemporalDenoise_;
    if (automaticDenoiseButton_) SetWindowTextW(automaticDenoiseButton_, automaticTemporalDenoise_ ? L"自动降噪：开*" : L"自动降噪：关");
    if (!automaticDetailAvailable()) {
        automaticDetailRestoration_ = false;
        if (detailAdjustmentEdit_ && controlText(detailAdjustmentEdit_) != L"0") SetWindowTextW(detailAdjustmentEdit_, L"0");
    }
    state_.automaticDetailRestoration = automaticDetailRestoration_;
    if (automaticDetailButton_) SetWindowTextW(automaticDetailButton_, automaticDetailRestoration_ ? L"自动细节：开*" : L"自动细节：关");
    if (!automaticCompressionAvailable()) {
        automaticCompressionRestoration_ = false;
        if (compressionAdjustmentEdit_ && controlText(compressionAdjustmentEdit_) != L"0") SetWindowTextW(compressionAdjustmentEdit_, L"0");
    }
    state_.automaticCompressionRestoration = automaticCompressionRestoration_;
    if (automaticCompressionButton_) SetWindowTextW(automaticCompressionButton_, automaticCompressionRestoration_ ? L"自动压缩：开*" : L"自动压缩：关");
    const int priorSize = static_cast<int>(SendMessageW(outputSizeCombo_, CB_GETCURSEL, 0, 0));
    if (SendMessageW(outputSizeCombo_, CB_GETCOUNT, 0, 0) >= 3) {
        SendMessageW(outputSizeCombo_, CB_DELETESTRING, 2, 0);
        SendMessageW(outputSizeCombo_, CB_DELETESTRING, 1, 0);
        const auto size1440 = outputTargetLabel(1);
        const auto size2160 = outputTargetLabel(2);
        SendMessageW(outputSizeCombo_, CB_INSERTSTRING, 1, reinterpret_cast<LPARAM>(size1440.c_str()));
        SendMessageW(outputSizeCombo_, CB_INSERTSTRING, 2, reinterpret_cast<LPARAM>(size2160.c_str()));
        SendMessageW(outputSizeCombo_, CB_SETCURSEL, std::clamp(priorSize, 0, 2), 0);
    }
    const int restorationIndex = static_cast<int>(SendMessageW(restorationCombo_, CB_GETCURSEL, 0, 0));
    const bool hasModel = restorationIndex > 0 && restorationIndex <= static_cast<int>(spatialRestorationModels_.size());
    const bool supportsFixedSize = hasModel && isUpscaleRestoration(spatialRestorationModels_[static_cast<std::size_t>(restorationIndex - 1)]);
    const bool editable = modelChangesAllowed();
    EnableWindow(automaticDenoiseButton_, editable);
    EnableWindow(denoiseAdjustmentEdit_, editable && automaticTemporalDenoise_);
    EnableWindow(denoiseAdjustmentResetButton_, editable && automaticTemporalDenoise_);
    EnableWindow(temporalRestorationCombo_, editable && !temporalRestorationModels_.empty());
    EnableWindow(deblurRestorationCombo_, editable && !deblurRestorationModels_.empty());
    EnableWindow(automaticDetailButton_, editable);
    EnableWindow(detailAdjustmentEdit_, editable && automaticDetailRestoration_);
    EnableWindow(detailAdjustmentResetButton_, editable && automaticDetailRestoration_);
    const int compressionIndex = compressionRestorationCombo_ ? static_cast<int>(SendMessageW(compressionRestorationCombo_, CB_GETCURSEL, 0, 0)) : 0;
    const bool compressionSelected = compressionIndex > 0 && compressionIndex <= static_cast<int>(compressionRestorationModels_.size());
    EnableWindow(compressionRestorationCombo_, editable && !compressionRestorationModels_.empty());
    EnableWindow(compressionStrengthCombo_, editable && compressionSelected && !automaticCompressionRestoration_);
    EnableWindow(compressionTilingCheck_, editable && compressionSelected);
    EnableWindow(automaticCompressionButton_, editable);
    EnableWindow(compressionAdjustmentEdit_, editable && automaticCompressionRestoration_);
    EnableWindow(compressionAdjustmentResetButton_, editable && automaticCompressionRestoration_);
    if (!compressionSelected && compressionStrengthCombo_) SendMessageW(compressionStrengthCombo_, CB_SETCURSEL, 0, 0);
    if (!compressionSelected && compressionTilingCheck_) SendMessageW(compressionTilingCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
    EnableWindow(restorationCombo_, editable && !spatialRestorationModels_.empty());
    EnableWindow(outputSizeCombo_, editable && supportsFixedSize &&
        (hasBatchSelection() || (sourceInspected_ && sourceWidth_ > 0 && sourceHeight_ > 0)));
    if (!hasModel || !supportsFixedSize) {
        SendMessageW(outputSizeCombo_, CB_SETCURSEL, 0, 0);
        return;
    }
    // Keep an unsupported selection visible together with its explicit reason.
    // Readiness blocks submission rather than silently changing the user's
    // selected dimensions back to the source size.
}

void NativeApp::inspectSource() {
    if (!processingService_) return;
    const auto source = currentSource();
    std::error_code sourceError;
    if (source.empty() || hasBatchSelection() || !std::filesystem::is_regular_file(source, sourceError)) {
        KillTimer(window_, kInspectionRetryTimer);
        if (processingService_->busy()) processingService_->cancel();
        return;
    }
    if (inspectionPending_ && activeInspectionGeneration_ == sourceGeneration_) return;
    if (processingService_->busy()) {
        processingService_->cancel();
        SetTimer(window_, kInspectionRetryTimer, 100, nullptr);
        state_.status = L"正在切换视频并重新检查…";
        return;
    }
    KillTimer(window_, kInspectionRetryTimer);
    const auto sourceValue = utf8(source);
    const auto generation = sourceGeneration_;
    if (!processingService_->inspect(sourceValue, [this, generation](application::ProcessingEvent event) {
        postProcessingEvent(std::move(event), generation);
    })) {
        SetTimer(window_, kInspectionRetryTimer, 100, nullptr);
        state_.status = L"正在等待之前的检查退出…";
    } else {
        activeInspectionGeneration_ = generation;
        inspectionPending_ = true;
    }
}

void NativeApp::postProcessingEvent(application::ProcessingEvent event, std::uint64_t generation) {
    const HWND callbackWindow = callbackWindow_.load();
    if (!callbackWindow || closing_) return;
    auto* payload = new NativeEvent{std::move(event),generation};
    if (!PostMessageW(callbackWindow, kNativeEventMessage, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
}

void NativeApp::handleProcessingEvent(application::ProcessingEvent event) {
    if (event.kind == application::ProcessingEvent::Kind::inspected && event.media) {
        sourceInspected_ = true;
        sourceColorSupported_ = event.sourceColorSupported;
        sourceInputProfile_ = event.sourceInputProfile;
        sourceColor_ = {};
        deviceAvailable_ = event.device.has_value();
        inspectedSource_ = controlText(sourceEdit_);
        if (const auto selected = std::find_if(event.media->streams.begin(), event.media->streams.end(), [&](const auto& stream) {
            return stream.index == event.media->preferredVideoStream && stream.type == "video";
        }); selected != event.media->streams.end()) sourceHeight_ = selected->height > 0 ? selected->height : 1080;
        if (const auto selected = std::find_if(event.media->streams.begin(), event.media->streams.end(), [&](const auto& stream) {
            return stream.index == event.media->preferredVideoStream && stream.type == "video";
        }); selected != event.media->streams.end()) {
            sourceWidth_ = selected->width > 0 ? selected->width : 1920;
            sourceColor_ = selected->color;
        }
        mediaSummary_ = streamSummary(*event.media);
        if (event.device) {
            deviceSummary_ = wideFromUtf8(event.device->name);
            deviceSummary_ += L"  ·  " + prettyBytes(event.device->totalBytes);
        } else deviceSummary_ = L"CUDA 不可用";
        bool encoderUsable = false;
        encoderCapabilities_ = event.encoderCapabilities;
        encoderCapabilitiesYuv420p_ = event.encoderCapabilitiesYuv420p;
        encoderAvailable_ = false;
        if (!event.encoderCapabilities.empty()) {
            for (const auto& capability : event.encoderCapabilities) {
                if (capability.contextOpened) { encoderUsable = true; break; }
            }
            if (!encoderUsable) for (const auto& capability : event.encoderCapabilitiesYuv420p) {
                if (capability.contextOpened) { encoderUsable = true; break; }
            }
            encoderAvailable_ = encoderUsable;
            state_.encoderSummary = L"自动（将优先尝试 H.264 NVENC）";
            if (!encoderUsable) mediaSummary_ += L"  ·  当前构建没有可用视频编码器";
        }
        if (!sourceColorSupported_) mediaSummary_ += L"  ·  该色彩配置没有经过 RIFE SDR 校验";
        if (!deviceAvailable_) mediaSummary_ += L"  ·  未检测到可用 CUDA 设备";
        state_.stage.clear();
        state_.status = evaluateStartReadiness().status;
    } else if (event.kind == application::ProcessingEvent::Kind::prepared) {
        state_.stage = processingStageLabel(event.stage);
        state_.status = L"模型已加载：" + wideFromUtf8(event.identity ? event.identity->backend : "unknown");
        if (!event.detail.empty()) state_.status += L" · " + wideFromUtf8(event.detail);
    } else if (event.kind == application::ProcessingEvent::Kind::progress) {
        state_.processing = true;
        state_.stage = processingStageLabel(event.stage);
        if (!event.encoder.empty()) state_.encoderSummary = wideFromUtf8(event.encoder);
        if (event.stage == "paused") {
            queuePaused_ = true;
            queuePauseTransitionPending_ = false;
            state_.stage = L"已暂停";
            state_.status = L"已暂停；点击“继续”恢复当前任务。本次暂停不等同于重启后的断点恢复";
        } else if (event.stage == "resumed") {
            queuePaused_ = false;
            queuePauseTransitionPending_ = false;
            state_.stage = L"继续处理";
            state_.status = L"已继续当前任务";
        } else if (event.stage == "encoder_fallback") {
            state_.status = L"编码器已回退到 " + wideFromUtf8(event.encoder) + L"：" + wideFromUtf8(event.detail);
        } else {
            state_.status = event.detail.empty() ? processingStageLabel(event.stage) : wideFromUtf8(event.detail);
        }
        state_.outputFrames = event.outputFrames;
        state_.processingFps = event.processingFps;
    } else if (event.kind == application::ProcessingEvent::Kind::completed) {
        state_.processing = false;
        state_.stage = L"completed";
        if (event.result) {
            state_.encoderSummary = wideFromUtf8(event.result->video.encoder);
            if (!event.result->encoderFallbackReason.empty()) {
                state_.encoderSummary += L"（已回退：" + wideFromUtf8(event.result->encoderFallbackReason) + L"）";
            }
        }
        state_.status = L"处理完成，输出已通过时间轴和文件完整性验证";
        state_.outputFrames = event.result ? event.result->video.framesSubmitted : state_.outputFrames;
        state_.processingFps = event.result ? event.result->processingFps : state_.processingFps;
    } else if (event.kind == application::ProcessingEvent::Kind::cancelled) {
        state_.processing = false;
        state_.stage = L"cancelled";
        state_.status = L"任务已取消";
    } else if (event.kind == application::ProcessingEvent::Kind::failed) {
        state_.processing = false;
        state_.stage = L"failed";
        state_.status = L"处理失败：" + wideFromUtf8(event.detail);
        if (!event.encoder.empty()) state_.encoderSummary = wideFromUtf8(event.encoder);
        else state_.encoderSummary = L"未打开（请求：" + controlText(encoderCombo_) + L"）";
    }
    updateView();
}
#endif

LRESULT CALLBACK NativeApp::comboProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto oldProc = reinterpret_cast<WNDPROC>(GetPropW(window, L"RifeLoveComboProc"));
    auto* app = reinterpret_cast<NativeApp*>(GetPropW(window, L"RifeLoveComboOwner"));
    if (!oldProc) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
        if (!dc) return 0;
        ScopedDcState saved(dc);
        RECT rect{};
        GetClientRect(window, &rect);
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool focused = GetFocus() == window;
        const COLORREF fill = enabled ? RGB(20, 25, 34) : RGB(23, 28, 37);
        const COLORREF border = focused ? RGB(162, 231, 203) : RGB(45, 55, 70);
        const COLORREF textColor = enabled ? RGB(230, 238, 247) : RGB(127, 142, 159);
        // Repaint the entire child surface before the rounded field, including
        // the corners exposed during focus, enable and resize transitions.
        HBRUSH background = CreateSolidBrush(RGB(19, 24, 32));
        FillRect(dc, &rect, background);
        DeleteObject(background);
        HBRUSH brush = CreateSolidBrush(fill);
        const float scale = app ? app->dpiScale() : 1.0F;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, scaled(1, scale)), border);
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, scaled(8, scale), scaled(8, scale));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        int index = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
        std::wstring label;
        if (index >= 0) {
            const LRESULT length = SendMessageW(window, CB_GETLBTEXTLEN, index, 0);
            if (length > 0) {
                label.resize(static_cast<std::size_t>(length));
                SendMessageW(window, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()));
            }
        }
        label = compactClosedSelectorLabel(label);
        RECT textRect = rect;
        textRect.left += scaled(12, scale);
        textRect.right -= scaled(30, scale);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, textColor);
        if (app && app->font_) SelectObject(dc, app->font_);
        DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        POINT arrow[3]{{rect.right - scaled(18, scale), (rect.top + rect.bottom) / 2 - scaled(2, scale)},
            {rect.right - scaled(10, scale), (rect.top + rect.bottom) / 2 - scaled(2, scale)},
            {rect.right - scaled(14, scale), (rect.top + rect.bottom) / 2 + scaled(3, scale)}};
        HBRUSH arrowBrush = CreateSolidBrush(textColor);
        const auto oldArrowBrush = SelectObject(dc, arrowBrush);
        const auto oldArrowPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arrow, 3);
        SelectObject(dc, oldArrowPen);
        SelectObject(dc, oldArrowBrush);
        DeleteObject(arrowBrush);
        if (focused) {
            RECT focus = rect;
            InflateRect(&focus, -scaled(3, scale), -scaled(3, scale));
            DrawFocusRect(dc, &focus);
        }
        saved.restore();
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(window, nullptr, FALSE);
        break;
    case CB_SETCURSEL: {
        const LRESULT result = CallWindowProcW(oldProc, window, message, wParam, lParam);
        InvalidateRect(window, nullptr, FALSE);
        return result;
    }
    case CB_SHOWDROPDOWN: {
        const LRESULT result = CallWindowProcW(oldProc, window, message, wParam, lParam);
        if (wParam != FALSE && app) app->resizeComboPopup(window);
        return result;
    }
    case WM_NCDESTROY:
        RemovePropW(window, L"RifeLoveComboProc");
        RemovePropW(window, L"RifeLoveComboOwner");
        break;
    default:
        break;
    }
    return CallWindowProcW(oldProc, window, message, wParam, lParam);
}

LRESULT CALLBACK NativeApp::checkboxProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const auto original = reinterpret_cast<WNDPROC>(GetPropW(window, L"RifeLoveCheckboxProc"));
    auto* app = reinterpret_cast<NativeApp*>(GetPropW(window, L"RifeLoveCheckboxOwner"));
    if (!original || !app) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
        if (!dc) return 0;
        ScopedDcState saved(dc);
        RECT rect{}; GetClientRect(window, &rect);
        FillRect(dc, &rect, app->staticBrush_);
        const float s = app->dpiScale();
        const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const int top = (rect.bottom - scaled(20, s)) / 2;
        HBRUSH fill = CreateSolidBrush(checked ? RGB(162, 231, 203) : RGB(48, 58, 73));
        const auto oldBrush = SelectObject(dc, fill);
        const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, 0, top, scaled(36, s), top + scaled(20, s), scaled(20, s), scaled(20, s));
        HBRUSH knob = CreateSolidBrush(checked ? RGB(19, 44, 35) : RGB(157, 171, 191));
        SelectObject(dc, knob);
        const int x = scaled(checked ? 18 : 2, s);
        Ellipse(dc, x, top + scaled(2, s), x + scaled(16, s), top + scaled(18, s));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(fill); DeleteObject(knob);
        rect.left += scaled(46, s);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? RGB(238, 242, 249) : RGB(105, 118, 136));
        SelectObject(dc, app->font_);
        DrawTextW(dc, app->controlText(window).c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (GetFocus() == window) DrawFocusRect(dc, &rect);
        saved.restore();
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    const auto result = CallWindowProcW(original, window, message, wParam, lParam);
    if (message == BM_SETCHECK || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, L"RifeLoveCheckboxProc");
        RemovePropW(window, L"RifeLoveCheckboxOwner");
    }
    return result;
}

LRESULT CALLBACK NativeApp::windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    NativeApp* app = reinterpret_cast<NativeApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<NativeApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window_ = window;
        app->callbackWindow_.store(window);
    }
    return app ? app->handleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT NativeApp::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        createControls();
        return 0;
    case WM_NOTIFY: {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
        if (info && info->hdr.code == TTN_GETDISPINFOW) {
            for (const auto edit : {sourceEdit_, outputEdit_}) {
                if (!edit || info->hdr.hwndFrom != GetPropW(edit, kPathTooltipProperty)) continue;
                auto* text = reinterpret_cast<std::wstring*>(GetPropW(edit, kPathTooltipTextProperty));
                if (text) {
                    *text = pathForEdit(edit).wstring();
                    if (text->empty()) *text = L"尚未设置路径";
                    info->lpszText = text->data();
                }
                return 0;
            }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    case WM_TIMER:
        if (wParam == kInspectionRetryTimer) {
            inspectSource();
            updateView();
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
#endif
    case WM_SIZE:
        layoutControls();
        renderer_.discardTarget();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        scrollSettings(-GET_WHEEL_DELTA_WPARAM(wParam) / 3);
        return 0;
    case WM_VSCROLL: {
        const int action = LOWORD(wParam);
        if (action == SB_LINEUP) scrollSettings(-32);
        else if (action == SB_LINEDOWN) scrollSettings(32);
        else if (action == SB_PAGEUP) scrollSettings(-180);
        else if (action == SB_PAGEDOWN) scrollSettings(180);
        else if (action == SB_TOP) { state_.scrollOffset = 0; scrollSettings(0); }
        else if (action == SB_BOTTOM) { state_.scrollOffset = 100000; scrollSettings(0); }
        else if (action == SB_THUMBTRACK || action == SB_THUMBPOSITION) {
            SCROLLINFO info{};
            info.cbSize = sizeof(SCROLLINFO); info.fMask = SIF_TRACKPOS;
            GetScrollInfo(window_, SB_VERT, &info);
            state_.scrollOffset = static_cast<float>(info.nTrackPos);
            scrollSettings(0);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        if (info) {
            const float s = dpiScale();
            const RECT work = monitorWorkArea(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
            info->ptMinTrackSize.x = std::min<LONG>(scaled(480, s), std::max<LONG>(1, work.right - work.left));
            info->ptMinTrackSize.y = std::min<LONG>(scaled(530, s), std::max<LONG>(1, work.bottom - work.top));
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            const RECT work = monitorWorkArea(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
            const RECT fitted = fitWindowToWorkArea(*suggested, work);
            SetWindowPos(window_, nullptr, fitted.left, fitted.top,
                fitted.right - fitted.left, fitted.bottom - fitted.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        setFonts();
        resizeForAdvancedSettings();
        layoutControls();
        return 0;
    }
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wParam);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        // Drag-and-drop must honor the same lock as the source controls. The
        // queue owns an immutable job snapshot while a task is active.
        if ((processingService_ && processingService_->busy()) || queueSessionActive_.load()) {
            if (drop) DragFinish(drop);
            return 0;
        }
#endif
        const UINT count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
        if (drop && count > 1) {
            std::vector<std::filesystem::path> inputs;
            wchar_t path[32768]{};
            for (UINT index = 0; index < count; ++index) {
                if (DragQueryFileW(drop, index, path, static_cast<UINT>(std::size(path))))
                    inputs.emplace_back(path);
            }
            if (!inputs.empty()) {
                queuedInputs_ = std::move(inputs);
                SetWindowTextW(sourceEdit_, queuedInputs_.front().c_str());
                const auto first = queuedInputs_.front();
                std::error_code sourceError;
                const auto outputDirectory = std::filesystem::is_directory(first, sourceError) ? first : first.parent_path();
                setOutputPath(outputDirectory);
                sourceInspected_ = false;
                sourceColorSupported_ = false;
                encoderAvailable_ = false;
                encoderCapabilities_.clear();
                encoderCapabilitiesYuv420p_.clear();
                deviceAvailable_ = false;
                mediaSummary_.clear();
                state_.encoderSummary.clear();
                state_.status = L"已载入 " + std::to_wstring(queuedInputs_.size()) + L" 个项目；点击开始后批量检查并加入本地队列";
                updateView();
            }
        } else
#endif
        if (drop && count > 0) {
            wchar_t path[32768]{};
            if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)))) setSourcePath(std::filesystem::path(path));
        }
        if (drop) DragFinish(drop);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        if (!renderer_.render(window_, state_)) {
            FillRect(paint.hdc, &paint.rcPaint, backgroundBrush_);
            SetTextColor(paint.hdc, RGB(242, 247, 255));
            SetBkMode(paint.hdc, TRANSPARENT);
            DrawTextW(paint.hdc, L"RifeLove 原生桌面", -1, &paint.rcPaint, DT_LEFT | DT_TOP);
        }
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (dc && !renderer_.renderToDc(window_, dc, state_)) {
            RECT client{};
            GetClientRect(window_, &client);
            FillRect(dc, &client, backgroundBrush_);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        if (HIWORD(wParam) == CBN_DROPDOWN) {
            resizeComboPopup(reinterpret_cast<HWND>(lParam));
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case kSelectSource: selectSource(); return 0;
            case kSelectSourceFolder: selectSourceFolder(); return 0;
            case kSelectOutput: selectOutput(); return 0;
            case kSelectModel: selectModel(); return 0;
            case kSelectRuntime: selectRuntime(); return 0;
            case kStart: startProcessing(); return 0;
            case kCancel: cancelProcessing(); return 0;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            case kComparison: showComparison(); return 0;
            case kAutomaticDenoise: configureAutomaticDenoise(); return 0;
            case kAutomaticDetail: configureAutomaticDetail(); return 0;
            case kAutomaticCompression: configureAutomaticCompression(); return 0;
            case kDenoiseAdjustmentReset:
                if (modelChangesAllowed() && automaticTemporalDenoise_) SetWindowTextW(denoiseAdjustmentEdit_, L"0");
                return 0;
            case kCompressionAdjustmentReset:
                if (modelChangesAllowed() && automaticCompressionRestoration_) SetWindowTextW(compressionAdjustmentEdit_, L"0");
                return 0;
            case kDetailAdjustmentReset:
                if (modelChangesAllowed() && automaticDetailRestoration_) SetWindowTextW(detailAdjustmentEdit_, L"0");
                return 0;
            case kTaskHistory: showTaskHistory(); return 0;
            case kRejectionDetails: {
                if (queueRejections_.empty()) return 0;
                const int width = scaled(650, dpiScale()), height = scaled(430, dpiScale());
                const HWND details = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"被拒绝的素材",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, width, height, window_, nullptr, instance_, nullptr);
                if (!details) return 0;
                RECT client{}; GetClientRect(details, &client);
                const HWND text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                    0, 0, client.right, client.bottom, details, nullptr, instance_, nullptr);
                if (!text) { DestroyWindow(details); return 0; }
                SendMessageW(text, EM_SETLIMITTEXT, 0x7ffffffe, 0);
                SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
                std::wstring report;
                for (const auto& item : queueRejections_) { report += item; report += L"\r\n\r\n"; }
                SetWindowTextW(text, report.c_str());
                SetFocus(text);
                return 0;
            }
#endif
            case kStatusDetails: showStatusDetails(); return 0;
            case kAdvancedToggle: toggleAdvancedSettings(); return 0;
            case kQualityDisclosure:
                qualityExpanded_ = !qualityExpanded_;
                SetWindowTextW(qualityDisclosureButton_, qualityExpanded_ ? L"收起画质设置  ▴" : L"展开画质设置  ▾");
                if (!qualityExpanded_) {
                    for (HWND combo : {temporalRestorationCombo_, deblurRestorationCombo_, restorationCombo_,
                            outputSizeCombo_, compressionRestorationCombo_, compressionStrengthCombo_})
                        SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
                }
                layoutControls();
                updateView();
                return 0;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            case kPauseResume: toggleQueuedPause(); return 0;
#endif
            default: break;
            }
        }
        if (LOWORD(wParam) == kSourceEdit && HIWORD(wParam) == EN_CHANGE) {
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
            ++sourceGeneration_;
            sourceInspected_ = false;
            sourceColorSupported_ = false;
            sourceInputProfile_.clear();
            encoderAvailable_ = false;
            deviceAvailable_ = false;
            state_.encoderSummary.clear();
            encoderCapabilities_.clear();
            encoderCapabilitiesYuv420p_.clear();
            const auto source = currentSource();
            if (queuedInputs_.empty() || queuedInputs_.front() != source) {
                queuedInputs_.clear();
                if (!source.empty()) queuedInputs_.push_back(source);
            }
            if (!source.empty() && currentOutput().empty()) {
                const int platformIndex = static_cast<int>(SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0));
                auto output = source.parent_path() / (source.stem().wstring() + L"_RifeLove");
                output.replace_extension(platformIndex > 0 ? L".mp4" : L".mkv");
                setOutputPath(output);
            }
            refreshReadinessStatus();
            inspectSource();
#endif
            updateView();
        }
        if (LOWORD(wParam) == kOutputEdit && HIWORD(wParam) == EN_CHANGE) {
            // A shortened display is backed by outputPath_. Once the user
            // edits the field, treat the visible text as the authoritative
            // path again instead of retaining the previous full path.
            if (outputPath_.empty() || controlText(outputEdit_) != outputPath_.wstring()) outputPath_.clear();
            refreshReadinessStatus();
            updateView();
        }
        if (LOWORD(wParam) == kRateCombo && HIWORD(wParam) == CBN_SELCHANGE) {
            layoutControls();
            refreshReadinessStatus();
            updateView();
        }
        if (LOWORD(wParam) == kPlatformCombo && HIWORD(wParam) == CBN_SELCHANGE) {
            applyPlatformPreset(static_cast<int>(SendMessageW(platformCombo_, CB_GETCURSEL, 0, 0)));
        }
        if ((LOWORD(wParam) == kMotionBlurCheck || LOWORD(wParam) == kCompressionTilingCheck) && HIWORD(wParam) == BN_CLICKED) {
            layoutControls();
            refreshReadinessStatus();
            updateView();
        }
        if ((LOWORD(wParam) == kTemporalRestorationCombo || LOWORD(wParam) == kDeblurRestorationCombo || LOWORD(wParam) == kCompressionRestorationCombo || LOWORD(wParam) == kCompressionStrengthCombo || LOWORD(wParam) == kRestorationCombo || LOWORD(wParam) == kOutputSizeCombo) && HIWORD(wParam) == CBN_SELCHANGE) {
            refreshReadinessStatus();
            updateView();
        }
        if ((LOWORD(wParam) == kRateCustomEdit || LOWORD(wParam) == kShutterEdit || LOWORD(wParam) == kSamplesEdit || LOWORD(wParam) == kDenoiseAdjustmentEdit || LOWORD(wParam) == kDetailAdjustmentEdit || LOWORD(wParam) == kCompressionAdjustmentEdit) && HIWORD(wParam) == EN_CHANGE) {
            refreshReadinessStatus();
            updateView();
        }
        if (LOWORD(wParam) == kEncoderCombo && HIWORD(wParam) == CBN_SELCHANGE) {
            refreshReadinessStatus();
            updateView();
        }
        return 0;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!item || !item->hwndItem) return 0;
        const ScopedDcState saved(item->hDC);
        if (item->CtlID == kMotionBlurCheck) {
            const bool enabled = IsWindowEnabled(item->hwndItem) != FALSE;
            const bool checked = SendMessageW(item->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const int scale = std::max(1, static_cast<int>(std::lround(dpiScale())));
            RECT toggle = item->rcItem;
            toggle.left += 1 * scale;
            toggle.top += std::max<LONG>(3, (toggle.bottom - toggle.top - 18 * scale) / 2);
            toggle.right = toggle.left + 34 * scale;
            toggle.bottom = toggle.top + 18 * scale;
            HBRUSH toggleBrush = CreateSolidBrush(checked ? RGB(125, 226, 188) : RGB(45, 61, 77));
            HPEN togglePen = CreatePen(PS_SOLID, 1, checked ? RGB(155, 240, 207) : RGB(77, 98, 116));
            const auto oldBrush = SelectObject(item->hDC, toggleBrush);
            const auto oldPen = SelectObject(item->hDC, togglePen);
            RoundRect(item->hDC, toggle.left, toggle.top, toggle.right, toggle.bottom, 18 * scale, 18 * scale);
            SelectObject(item->hDC, oldPen);
            SelectObject(item->hDC, oldBrush);
            DeleteObject(togglePen);
            DeleteObject(toggleBrush);
            HBRUSH knobBrush = CreateSolidBrush(checked ? RGB(16, 43, 42) : RGB(180, 194, 207));
            const auto oldKnobBrush = SelectObject(item->hDC, knobBrush);
            const int knob = 14 * scale;
            const int knobX = checked ? toggle.right - knob - 2 * scale : toggle.left + 2 * scale;
            Ellipse(item->hDC, knobX, toggle.top + 2 * scale, knobX + knob, toggle.bottom - 2 * scale);
            SelectObject(item->hDC, oldKnobBrush);
            DeleteObject(knobBrush);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, enabled ? RGB(224, 235, 243) : RGB(127, 142, 159));
            if (font_) SelectObject(item->hDC, font_);
            RECT textRect = item->rcItem;
            textRect.left = toggle.right + 9 * scale;
            DrawTextW(item->hDC, controlText(item->hwndItem).c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if ((item->itemState & ODS_FOCUS) != 0) {
                RECT focus = item->rcItem;
                InflateRect(&focus, -2, -2);
                DrawFocusRect(item->hDC, &focus);
            }
            return TRUE;
        }
        if (item->CtlType == ODT_COMBOBOX) {
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            const bool focused = (item->itemState & ODS_FOCUS) != 0;
            const COLORREF fill = selected ? RGB(36, 66, 82) : RGB(14, 23, 34);
            const COLORREF textColor = selected ? RGB(242, 247, 255) : RGB(220, 231, 242);
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            std::wstring label;
            if (item->itemID != static_cast<UINT>(-1)) {
                const LRESULT length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
                if (length > 0) {
                    label.resize(static_cast<std::size_t>(length));
                    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(label.data()));
                }
            }
            RECT textRect = item->rcItem;
            textRect.left += scaled(12, dpiScale());
            // Popup rows have no arrow: keep their full text width available.
            textRect.right -= scaled(12, dpiScale());
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, textColor);
            if (font_) SelectObject(item->hDC, font_);
            DrawTextW(item->hDC, label.c_str(), static_cast<int>(label.size()), &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (focused) {
                RECT focus = item->rcItem;
                InflateRect(&focus, -2, -2);
                DrawFocusRect(item->hDC, &focus);
            }
            return TRUE;
        }
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool pressed = !disabled && (item->itemState & ODS_SELECTED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;
        const bool accentButton = item->CtlID == kStart || item->CtlID == kSelectSource || item->CtlID == kSelectOutput;
        const COLORREF color = disabled ? RGB(38, 49, 63) : accentButton ?
            (pressed ? RGB(118, 202, 170) : RGB(145, 224, 192)) :
            (pressed ? RGB(43, 74, 101) : RGB(34, 56, 82));
        const COLORREF textColor = disabled ? RGB(125, 140, 157) : accentButton ? RGB(16, 36, 44) : RGB(242, 247, 255);
        const bool sidebarButton = item->CtlID == kSelectModel || item->CtlID == kSelectRuntime;
        const bool dockButton = item->CtlID == kStart || item->CtlID == kPauseResume || item->CtlID == kCancel;
        HBRUSH surface = CreateSolidBrush(sidebarButton ? RGB(16, 20, 28) : dockButton ? RGB(12, 15, 21) : RGB(19, 24, 32));
        FillRect(item->hDC, &item->rcItem, surface);
        DeleteObject(surface);
        const int inset = pressed ? scaled(1, dpiScale()) : 0;
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, scaled(1, dpiScale())), disabled ? RGB(48, 61, 76) :
            focused ? RGB(162, 231, 203) : RGB(67, 91, 111));
        const auto oldBrush = SelectObject(item->hDC, brush);
        const auto oldPen = SelectObject(item->hDC, pen);
        RECT rect = item->rcItem;
        InflateRect(&rect, -inset, -inset);
        RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, scaled(8, dpiScale()), scaled(8, dpiScale()));
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, textColor);
        if (font_) SelectObject(item->hDC, font_);
        const auto label = controlText(item->hwndItem);
        InflateRect(&rect, -scaled(8, dpiScale()), 0);
        DrawTextW(item->hDC, label.c_str(), static_cast<int>(label.size()), &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if ((item->itemState & ODS_FOCUS) != 0) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(item->hDC, &focus);
        }
        return TRUE;
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure && measure->CtlType == ODT_COMBOBOX) measure->itemHeight = static_cast<UINT>(scaled(30, dpiScale()));
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(159, 178, 201));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(18, 30, 45));
        return reinterpret_cast<LRESULT>(staticBrush_);
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 247, 255));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(14, 23, 34));
        return reinterpret_cast<LRESULT>(editBrush_);
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(220, 231, 242));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(14, 23, 34));
        return reinterpret_cast<LRESULT>(editBrush_);
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(242, 247, 255));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(18, 30, 45));
        return reinterpret_cast<LRESULT>(editBrush_);
    case WM_CLOSE:
        closeWindow();
        return 0;
    case WM_NCDESTROY:
        callbackWindow_.store(nullptr);
        PostQuitMessage(0);
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        return DefWindowProcW(window_, message, wParam, lParam);
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    case kNativeEventMessage: {
        auto* payload = reinterpret_cast<NativeEvent*>(lParam);
        if (payload) {
            if (payload->generation == activeInspectionGeneration_) inspectionPending_ = false;
            if (payload->generation == sourceGeneration_ && !hasBatchSelection())
                handleProcessingEvent(std::move(payload->event));
            delete payload;
        }
        return 0;
    }
    case kHistoryLoadedMessage: {
        auto* payload = reinterpret_cast<NativeHistoryResult*>(lParam);
        if (payload) {
            handleHistoryResult(std::move(*payload));
            delete payload;
        }
        return 0;
    }
    case kQueueEventMessage: {
        auto* payload = reinterpret_cast<NativeQueueEvent*>(lParam);
        if (payload) {
            handleQueueEvent(std::move(payload->event));
            delete payload;
        }
        return 0;
    }
    case kQueueNoticeMessage: {
        auto* payload = reinterpret_cast<NativeQueueNotice*>(lParam);
        if (payload) {
            handleQueueNotice(std::move(*payload));
            delete payload;
        }
        return 0;
    }
#endif
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

} // namespace rifelove::native_desktop

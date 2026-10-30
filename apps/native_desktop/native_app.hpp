#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_renderer.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "application/import_service.hpp"
#include "application/processing_service.hpp"
#include "application/task_queue.hpp"
#include "inference/model_pack.hpp"
#include "restoration/model_pack.hpp"
#endif

namespace rifelove::native_desktop {

class NativeApp final {
public:
    NativeApp();
    ~NativeApp();
    NativeApp(const NativeApp&) = delete;
    NativeApp& operator=(const NativeApp&) = delete;

    [[nodiscard]] bool create(HINSTANCE instance, int showCommand);
    [[nodiscard]] int run();
#ifdef RIFELOVE_NATIVE_TESTING
    [[nodiscard]] const NativeRenderState& testViewState() const noexcept { return state_; }
#endif

private:
    static constexpr UINT kNativeEventMessage = WM_APP + 41;
    static constexpr UINT_PTR kInspectionRetryTimer = 41;
    static constexpr UINT kQueueEventMessage = WM_APP + 42;
    static constexpr UINT kQueueNoticeMessage = WM_APP + 43;
    static constexpr UINT kHistoryLoadedMessage = WM_APP + 44;
    static constexpr int kSourceEdit = 1001;
    static constexpr int kSelectSource = 1002;
    static constexpr int kOutputEdit = 1003;
    static constexpr int kSelectOutput = 1004;
    static constexpr int kRateCombo = 1005;
    static constexpr int kEncoderCombo = 1006;
    static constexpr int kSelectModel = 1007;
    static constexpr int kSelectRuntime = 1008;
    static constexpr int kStart = 1009;
    static constexpr int kCancel = 1010;
    static constexpr int kPauseResume = 1023;
    static constexpr int kRateCustomEdit = 1011;
    static constexpr int kPlatformCombo = 1012;
    static constexpr int kMotionBlurCheck = 1013;
    static constexpr int kShutterEdit = 1014;
    static constexpr int kSamplesEdit = 1015;
    static constexpr int kAdvancedToggle = 1016;
    // 1017 is kept as the spatial restoration selector for compatibility with
    // existing UI automation and queued task expectations.
    static constexpr int kRestorationCombo = 1017;
    static constexpr int kOutputSizeCombo = 1018;
    static constexpr int kRejectionDetails = 1019;
    static constexpr int kTaskHistory = 1020;
    static constexpr int kHistoryRefresh = 1021;
    static constexpr int kSelectSourceFolder = 1022;
    static constexpr int kTemporalRestorationCombo = 1024;
    static constexpr int kShutterLabel = 1106;
    static constexpr int kSamplesLabel = 1107;
    static constexpr int kRestorationLabel = 1109;
    static constexpr int kOutputSizeLabel = 1110;
    static constexpr int kTemporalRestorationLabel = 1111;

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    struct NativeEvent final {
        application::ProcessingEvent event;
        std::uint64_t generation = 0;
    };
    struct NativeQueueEvent final {
        application::QueueEvent event;
    };
    struct NativeQueueNotice final {
        std::wstring status;
        std::size_t accepted = 0;
        std::size_t rejected = 0;
        bool importFinished = false;
        bool ownerFinished = false;
        bool failed = false;
        std::wstring rejectionDetail{};
        std::vector<std::wstring> rejectionList{};
    };
#endif

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK comboProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK checkboxProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK pathEditProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layoutControls();
    void setFonts();
    void styleCombo(HWND combo);
    void updateView();
    void updateButtonState();
    void toggleAdvancedSettings();
    void resizeForAdvancedSettings();
    void scrollSettings(int delta);
    void selectSource();
    void selectSourceFolder();
    void selectOutput();
    void selectModel();
    void selectRuntime();
    void setSourcePath(const std::filesystem::path& source);
    void setOutputPath(const std::filesystem::path& output);
    void applyPlatformPreset(int index);
    void startProcessing();
    void cancelProcessing();
    void closeWindow();
    [[nodiscard]] std::filesystem::path browseForFolder(const wchar_t* title) const;
    [[nodiscard]] std::filesystem::path currentSource() const;
    [[nodiscard]] std::filesystem::path currentOutput() const;
    [[nodiscard]] std::wstring controlText(HWND control) const;
    [[nodiscard]] std::filesystem::path pathForEdit(HWND edit) const;
    void showPathContextMenu(HWND edit, POINT screenPoint);
    void showFullPath(HWND edit);
    void subclassPathEdit(HWND edit);
    struct StartReadiness final {
        bool ready = false;
        std::wstring status;
    };
    struct OutputTargetCheck final {
        int width = 0;
        int height = 0;
        bool selected = false;
        bool aspectMatches = false;
        bool generatedEnough = false;
        bool dimensionsEven = false;

        [[nodiscard]] bool usable() const noexcept {
            return selected && aspectMatches && generatedEnough && dimensionsEven;
        }
    };
    [[nodiscard]] StartReadiness evaluateStartReadiness() const;
    [[nodiscard]] OutputTargetCheck outputTargetCheck(int selection) const;
    [[nodiscard]] std::wstring outputTargetLabel(int selection) const;
    void refreshReadinessStatus();
    [[nodiscard]] static std::string utf8(const std::filesystem::path& path);
    [[nodiscard]] static std::wstring pathText(const std::filesystem::path& path);
    [[nodiscard]] float dpiScale() const;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    struct NativeHistoryResult final {
        std::uint64_t generation = 0;
        std::wstring text{};
        std::wstring status{};
    };

    static LRESULT CALLBACK historyWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void inspectSource();
    void handleProcessingEvent(application::ProcessingEvent event);
    void postProcessingEvent(application::ProcessingEvent event, std::uint64_t generation);
    void handleQueueEvent(application::QueueEvent event);
    void handleQueueNotice(NativeQueueNotice notice);
    void postQueueEvent(application::QueueEvent event);
    void postQueueNotice(NativeQueueNotice notice);
    void startQueuedProcessing(application::ProcessingJob job, std::vector<std::filesystem::path> inputs, int platformIndex);
    void stopQueuedProcessing();
    void toggleQueuedPause();
    [[nodiscard]] bool hasBatchSelection() const noexcept;
    void discoverLocalResources();
    [[nodiscard]] bool refreshModel(const std::filesystem::path& directory);
    [[nodiscard]] bool refreshRuntime(const std::filesystem::path& directory);
    void discoverRestorationModels();
    void updateRestorationControls();
    void showTaskHistory();
    void refreshTaskHistory();
    void handleHistoryResult(NativeHistoryResult result);
#endif

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND sourceEdit_ = nullptr;
    HWND sourceButton_ = nullptr;
    HWND sourceFolderButton_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND outputButton_ = nullptr;
    HWND rateCombo_ = nullptr;
    HWND rateCustomEdit_ = nullptr;
    HWND encoderCombo_ = nullptr;
    HWND platformCombo_ = nullptr;
    HWND motionBlurCheck_ = nullptr;
    HWND shutterEdit_ = nullptr;
    HWND samplesEdit_ = nullptr;
    HWND advancedButton_ = nullptr;
    HWND temporalRestorationCombo_ = nullptr;
    HWND restorationCombo_ = nullptr;
    HWND outputSizeCombo_ = nullptr;
    HWND shutterLabel_ = nullptr;
    HWND samplesLabel_ = nullptr;
    HWND restorationLabel_ = nullptr;
    HWND outputSizeLabel_ = nullptr;
    HWND temporalRestorationLabel_ = nullptr;
    HWND modelButton_ = nullptr;
    HWND runtimeButton_ = nullptr;
    HWND startButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND pauseResumeButton_ = nullptr;
    HWND rejectionDetailsButton_ = nullptr;
    HWND historyButton_ = nullptr;
    HWND sourceLabel_ = nullptr;
    HWND outputLabel_ = nullptr;
    HWND rateLabel_ = nullptr;
    HWND encoderLabel_ = nullptr;
    HWND modelLabel_ = nullptr;
    HWND platformLabel_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH staticBrush_ = nullptr;
    NativeRenderer renderer_;
    NativeRenderState state_;
    std::wstring mediaSummary_;
    std::wstring modelSummary_;
    std::wstring runtimeSummary_;
    std::wstring deviceSummary_;
    std::wstring platformSummary_;
    int sourceHeight_ = 1080;
    int sourceWidth_ = 1920;
    std::filesystem::path applicationDirectory_;
    std::filesystem::path modelDirectory_;
    std::filesystem::path runtimeDirectory_;
    std::filesystem::path outputPath_;
    std::atomic<HWND> callbackWindow_{nullptr};
    std::atomic<bool> closing_{false};
    bool advancedExpanded_ = false;
    int localRateSelection_ = 1;
    int localEncoderSelection_ = 0;
    int lastPlatformSelection_ = 0;
    std::wstring localCustomRate_ = L"300/1";
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    HWND historyWindow_ = nullptr;
    HWND historyText_ = nullptr;
    HWND historyStatus_ = nullptr;
    HWND historyRefreshButton_ = nullptr;
    std::jthread historyThread_;
    std::atomic<bool> historyLoading_{false};
    std::uint64_t historyGeneration_ = 0;
    std::vector<restoration::ModelPack> temporalRestorationModels_;
    std::vector<restoration::ModelPack> spatialRestorationModels_;
    std::optional<restoration::ModelPack> restorationModelPack_;
    std::optional<restoration::ModelPack> temporalRestorationModelPack_;
#endif

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    std::unique_ptr<application::ProcessingService> processingService_;
    std::unique_ptr<application::TaskQueue> taskQueue_;
    std::jthread queueOwnerThread_;
    std::stop_source queueImportStop_;
    std::vector<std::filesystem::path> queuedInputs_;
    std::atomic<bool> queueSessionActive_{false};
    bool queuePaused_ = false;
    bool queuePauseTransitionPending_ = false;
    std::size_t queueAccepted_ = 0;
    std::size_t queueRejected_ = 0;
    bool queueHadFailures_ = false;
    std::wstring queueFirstRejection_;
    std::vector<std::wstring> queueRejections_;
    std::wstring queueSummary_;
    std::optional<inference::ModelPack> modelPack_;
    bool sourceInspected_ = false;
    bool sourceColorSupported_ = false;
    std::string sourceInputProfile_;
    bool encoderAvailable_ = false;
    std::vector<media::EncoderCapability> encoderCapabilities_;
    bool deviceAvailable_ = false;
    std::wstring inspectedSource_;
    std::uint64_t sourceGeneration_ = 0;
    std::uint64_t activeInspectionGeneration_ = 0;
    bool inspectionPending_ = false;
#endif
};

} // namespace rifelove::native_desktop

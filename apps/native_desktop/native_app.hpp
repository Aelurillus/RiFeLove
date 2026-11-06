#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_renderer.hpp"
#include "native_status_window.hpp"
#include "native_output_target.hpp"
#include "native_model_store.hpp"
#include "native_data_directory.hpp"
#include "native_compression_settings.hpp"
#include "native_dynamic_settings.hpp"
#include "core/pipeline/video_pipeline.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "native_comparison_window.hpp"
#include "application/import_service.hpp"
#include "application/processing_service.hpp"
#include "application/task_queue.hpp"
#include "inference/model_pack.hpp"
#include "restoration/model_pack.hpp"
#endif

namespace rifelove::native_desktop {

class NativeApp final {
public:
    explicit NativeApp(std::filesystem::path applicationDirectory = {}, std::filesystem::path localAppDataRoot = {});
    ~NativeApp();
    NativeApp(const NativeApp&) = delete;
    NativeApp& operator=(const NativeApp&) = delete;

    [[nodiscard]] bool create(HINSTANCE instance, int showCommand);
    [[nodiscard]] int run();
#ifdef RIFELOVE_NATIVE_TESTING
    [[nodiscard]] HWND testMainWindow() const noexcept { return window_; }
    [[nodiscard]] const NativeRenderState& testViewState() const noexcept { return state_; }
    bool testNavigateSettings(bool reverse = false) { return focusNextControl(reverse); }
    [[nodiscard]] PipelineSettings testExportSettings() const {
        PipelineSettings settings;
        applyExportControls(settings);
        return settings;
    }
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    [[nodiscard]] const auto& testLocalModels() const noexcept { return localModels_; }
    [[nodiscard]] const auto& testModelPreferences() const noexcept { return modelPreferences_; }
    [[nodiscard]] const auto& testDataDirectory() const noexcept { return dataDirectory_; }
    bool testRegisterModel(const std::filesystem::path& path) { return registerLocalModel(path); }
    bool testSetDefaultModel(std::size_t index) { return setDefaultLocalModel(index); }
    bool testUnregisterModel(std::size_t index) { return unregisterLocalModel(index); }
    void testRefreshModels() { refreshLocalModels(); }
    HWND testShowModelCenter() { showModelCenter(); return modelCenterWindow_; }
    HWND testShowComparison() { showComparison(); return comparisonWindow_ ? comparisonWindow_->window() : nullptr; }
    bool testSetAutomaticDenoise(bool enabled) { return setAutomaticDenoise(enabled); }
    bool testSetAutomaticCompression(bool enabled) { return setAutomaticCompression(enabled); }
    bool testSetAutomaticDetail(bool enabled) { return setAutomaticDetail(enabled); }
    // Exercise the real busy-state control policy without starting a worker.
    void testSetQueueControlsBusy(bool busy) { queueSessionActive_.store(busy); updateView(); }
    bool testCaptureRestorationDescriptors(application::ProcessingJob& job,
        const restoration::ModelPack* temporal, const restoration::ModelPack* spatial,
        const restoration::ModelPack* compression = nullptr, float strength = 0.0F, bool tiled = false,
        const restoration::ModelPack* deblur = nullptr) {
        return captureRestorationDescriptors(job, temporal, spatial, compression, strength, tiled, deblur);
    }
    bool testCaptureSelectedRestoration(application::ProcessingJob& job) { return captureSelectedRestoration(job); }
    // Descriptor-only fixture injection. It never creates an inference session
    // or claims the tiny synthetic graph is a qualified model.
    void testSetCompressionModels(std::vector<restoration::ModelPack> models) {
        compressionRestorationModels_ = std::move(models);
        syncRestorationSelectors();
        updateView();
    }
    void testSetSpatialModels(std::vector<restoration::ModelPack> models) {
        spatialRestorationModels_ = std::move(models);
        syncRestorationSelectors();
        updateView();
    }
    void testSetDeblurModels(std::vector<restoration::ModelPack> models) {
        deblurRestorationModels_ = std::move(models);
        syncRestorationSelectors();
        updateView();
    }
    void testSetBatchSelection(bool enabled) {
        queuedInputs_ = enabled ? std::vector<std::filesystem::path>{L"C:/fixture-a.mp4",L"C:/fixture-b.mp4"}
                                : std::vector<std::filesystem::path>{};
        sourceInspected_ = false; sourceWidth_ = 0; sourceHeight_ = 0;
        updateView();
    }
    void testSetSourceDimensions(int width, int height) {
        sourceWidth_ = width;
        sourceHeight_ = height;
        updateRestorationControls();
    }
#endif
#endif

private:
    static constexpr UINT kNativeEventMessage = WM_APP + 41;
    static constexpr UINT_PTR kInspectionRetryTimer = 41;
    static constexpr UINT_PTR kModelInstallTimer = 1208;
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
    static constexpr int kComparison = 1025;
    static constexpr int kAutomaticDenoise = 1026;
    static constexpr int kCompressionRestorationCombo = 1027;
    static constexpr int kCompressionStrengthCombo = 1028;
    static constexpr int kCompressionTilingCheck = 1029;
    static constexpr int kDenoiseAdjustmentEdit = 1030;
    static constexpr int kDenoiseAdjustmentReset = 1031;
    static constexpr int kAutomaticCompression = 1032;
    static constexpr int kCompressionAdjustmentEdit = 1033;
    static constexpr int kCompressionAdjustmentReset = 1034;
    static constexpr int kStatusDetails = 1035;
    static constexpr int kDeblurRestorationCombo = 1036;
    static constexpr int kAutomaticDetail = 1037;
    static constexpr int kDetailAdjustmentEdit = 1038;
    static constexpr int kDetailAdjustmentReset = 1039;
    static constexpr int kQualityDisclosure = 1040;
    static constexpr int kHome = 1041;
    static constexpr int kStudio = 1042;
    static constexpr int kHomeStart = 1043;
    static constexpr int kShutterLabel = 1106;
    static constexpr int kSamplesLabel = 1107;
    static constexpr int kRestorationLabel = 1109;
    static constexpr int kOutputSizeLabel = 1110;
    static constexpr int kTemporalRestorationLabel = 1111;
    static constexpr int kModelList = 1201;
    static constexpr int kModelDetails = 1202;
    static constexpr int kModelRegister = 1203;
    static constexpr int kModelDefault = 1204;
    static constexpr int kModelUnregister = 1205;
    static constexpr int kModelRefresh = 1206;
    static constexpr int kModelClose = 1207;
    static constexpr int kModelInstall = 1208;
    static constexpr int kModelCancelInstall = 1209;
    static constexpr int kModelDownload = 1210;
    static constexpr int kModelDownloadUrl = 1211;
    static constexpr int kModelListHeading = 1212;
    static constexpr int kModelDetailsHeading = 1213;
    static constexpr int kModelSearch = 1214;
    static constexpr int kModelOfficial = 1215;
    static constexpr int kHistoryList = 1208;
    static constexpr int kHistoryRetry = 1209;
    static constexpr int kHistoryResume = 1210;
    static constexpr int kHistoryRestart = 1211;

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
    static LRESULT CALLBACK scrollChildProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void createControls();
    void layoutControls();
    void resizeComboPopup(HWND combo) const;
    void setFonts();
    void styleCombo(HWND combo);
    bool focusNextControl(bool reverse);
    void ensureControlVisible(HWND control);
    void updateView();
    void setHomePage(bool visible);
    void updateButtonState();
    void showStatusDetails();
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
    void applyExportControls(PipelineSettings& settings) const;
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
    using OutputTargetCheck = NativeOutputTarget;
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
        std::wstring status{};
        std::string checkpointInspectionError{};
        std::vector<std::string> checkpointCandidateTaskIds{};
        std::vector<ConfiguredTask> tasks{};
    };
    enum class HistoryAction { retry, resumeCheckpoint, restartFromZero };

    static LRESULT CALLBACK historyWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void setHistoryFont(HWND window);
    void inspectSource();
    void handleProcessingEvent(application::ProcessingEvent event);
    void postProcessingEvent(application::ProcessingEvent event, std::uint64_t generation);
    void handleQueueEvent(application::QueueEvent event);
    void handleQueueNotice(NativeQueueNotice notice);
    void postQueueEvent(application::QueueEvent event);
    void postQueueNotice(NativeQueueNotice notice);
    void startQueuedProcessing(application::ProcessingJob job, std::vector<std::filesystem::path> inputs, int platformIndex, int outputTargetSelection);
    bool captureRestorationDescriptors(application::ProcessingJob& job,
        const restoration::ModelPack* temporal, const restoration::ModelPack* spatial,
        const restoration::ModelPack* compression = nullptr, float strength = 0.0F, bool tiled = false,
        const restoration::ModelPack* deblur = nullptr);
    bool captureSelectedRestoration(application::ProcessingJob& job);
    void stopQueuedProcessing();
    void toggleQueuedPause();
    [[nodiscard]] bool hasBatchSelection() const noexcept;
    void discoverLocalResources();
    [[nodiscard]] bool refreshModel(const std::filesystem::path& directory);
    [[nodiscard]] bool refreshRuntime(const std::filesystem::path& directory);
    void discoverRestorationModels();
    void updateRestorationControls();
    struct LocalModel final {
        std::filesystem::path directory;
        std::wstring name;
        std::wstring version;
        std::wstring modelSha256;
        std::wstring licenseId;
        std::wstring sourceRepository;
        std::wstring upstreamCommit;
        std::wstring licenseScopeEvidence;
        std::wstring numericalProfile;
        std::wstring validatedBackends;
        std::wstring capabilities;
        std::wstring profiles;
        std::wstring verification;
        int kind = -1;
        bool verified = false;
        bool usable = false;
    };
    [[nodiscard]] LocalModel inspectLocalModel(const std::filesystem::path& directory) const;
    void refreshLocalModels();
    void syncRestorationSelectors();
    [[nodiscard]] bool registerLocalModel(const std::filesystem::path& directory);
    [[nodiscard]] bool setDefaultLocalModel(std::size_t index);
    [[nodiscard]] bool unregisterLocalModel(std::size_t index);
    [[nodiscard]] bool saveModelPreferences(const NativeModelPreferences& preferences);
    [[nodiscard]] bool modelChangesAllowed() const;
    static LRESULT CALLBACK modelCenterWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void showModelCenter();
    void layoutModelCenter();
    void updateModelCenter();
    void updateModelDetails();
    struct ModelPackageMirror final {
        std::wstring archiveUrl;
        std::wstring manifestUrl;
        std::wstring channelName;
        std::uint64_t expectedManifestSizeBytes = 0;
        std::string expectedManifestSha256;
        std::string expectedPackageId;
        std::string expectedModelId;
        std::string expectedModelVersion;
        std::string expectedModelSha256;
    };
    void startModelPackageInstall(const std::filesystem::path& archive, std::vector<ModelPackageMirror> mirrors = {},
        bool fetchOfficialCatalog = false);
    void cancelModelPackageInstall();
    void pollModelPackageInstall();
    void showTaskHistory();
    void showComparison();
    void updateTaskHistorySelection();
    void operateOnHistoryTask(HistoryAction action);
    void configureAutomaticDenoise();
    bool setAutomaticDenoise(bool enabled);
    [[nodiscard]] bool automaticDenoiseAvailable() const;
    void configureAutomaticCompression();
    bool setAutomaticCompression(bool enabled);
    [[nodiscard]] bool automaticCompressionAvailable() const;
    void configureAutomaticDetail();
    bool setAutomaticDetail(bool enabled);
    [[nodiscard]] bool automaticDetailAvailable() const;
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
    HWND qualityDisclosureButton_ = nullptr;
    HWND temporalRestorationCombo_ = nullptr;
    HWND deblurRestorationCombo_ = nullptr;
    HWND compressionRestorationCombo_ = nullptr;
    HWND compressionStrengthCombo_ = nullptr;
    HWND compressionTilingCheck_ = nullptr;
    HWND restorationCombo_ = nullptr;
    HWND outputSizeCombo_ = nullptr;
    HWND shutterLabel_ = nullptr;
    HWND samplesLabel_ = nullptr;
    HWND restorationLabel_ = nullptr;
    HWND outputSizeLabel_ = nullptr;
    HWND temporalRestorationLabel_ = nullptr;
    HWND homeButton_ = nullptr;
    HWND studioButton_ = nullptr;
    HWND homeStartButton_ = nullptr;
    HWND modelButton_ = nullptr;
    HWND runtimeButton_ = nullptr;
    HWND startButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND pauseResumeButton_ = nullptr;
    HWND rejectionDetailsButton_ = nullptr;
    HWND statusDetailsButton_ = nullptr;
    HWND historyButton_ = nullptr;
    HWND comparisonButton_ = nullptr;
    HWND automaticDenoiseButton_ = nullptr;
    HWND denoiseAdjustmentEdit_ = nullptr;
    HWND denoiseAdjustmentResetButton_ = nullptr;
    HWND automaticDetailButton_ = nullptr;
    HWND detailAdjustmentEdit_ = nullptr;
    HWND detailAdjustmentResetButton_ = nullptr;
    HWND automaticCompressionButton_ = nullptr;
    HWND compressionAdjustmentEdit_ = nullptr;
    HWND compressionAdjustmentResetButton_ = nullptr;
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
    NativeStatusWindow statusWindow_;
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
    bool qualityExpanded_ = false;
#ifdef RIFELOVE_NATIVE_TESTING
    bool homePage_ = false;
#else
    bool homePage_ = true;
#endif
    bool automaticTemporalDenoise_ = false;
    bool automaticDetailRestoration_ = false;
    bool automaticCompressionRestoration_ = false;
    struct ExportPresetControls {
        int rateSelection = 1;
        int encoderSelection = 0;
        std::wstring customRate = L"300/1";
        bool blurEnabled = false;
        std::wstring shutterAngle = L"180";
        std::wstring sampleCount = L"4";
        bool saved = false;
    };
    std::array<ExportPresetControls, 6> exportPresetControls_{};
    int lastPlatformSelection_ = 0;
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
    std::unique_ptr<NativeComparisonWindow> comparisonWindow_;
    NativeModelPreferences modelPreferences_;
    NativeDataDirectory dataDirectory_;
    bool modelPreferencesWritable_ = true;
    std::wstring modelCenterNotice_;
    std::vector<LocalModel> localModels_;
    HWND modelCenterWindow_ = nullptr;
    HWND modelList_ = nullptr;
    HWND modelDetails_ = nullptr;
    HWND modelCenterStatus_ = nullptr;
    HWND modelDownloadUrl_ = nullptr;
    HWND modelSearch_ = nullptr;
    HWND modelListHeading_ = nullptr;
    HWND modelDetailsHeading_ = nullptr;
    HFONT modelCenterFont_ = nullptr;
    HFONT modelCenterHeadingFont_ = nullptr;
    HBRUSH modelCenterBackgroundBrush_ = nullptr;
    HBRUSH modelCenterSurfaceBrush_ = nullptr;
    std::vector<std::size_t> modelCenterVisibleIndexes_;
    std::filesystem::path modelCenterSelectedPath_;
    struct ModelPackageInstallState final {
        std::uint64_t completedBytes = 0;
        std::uint64_t totalBytes = 0;
        std::wstring status;
        std::optional<std::filesystem::path> installedDirectory;
        std::wstring failure;
        bool finished = false;
    };
    std::jthread modelInstallThread_;
    std::atomic<bool> modelInstallActive_{false};
    std::mutex modelInstallMutex_;
    ModelPackageInstallState modelInstallState_;
    bool closeModelCenterWhenInstallEnds_ = false;
    HWND historyWindow_ = nullptr;
    HWND historyList_ = nullptr;
    HWND historyText_ = nullptr;
    HWND historyStatus_ = nullptr;
    HWND historyRefreshButton_ = nullptr;
    HWND historyRetryButton_ = nullptr;
    HWND historyResumeButton_ = nullptr;
    HWND historyRestartButton_ = nullptr;
    HFONT historyFont_ = nullptr;
    std::vector<ConfiguredTask> historyRows_;
    std::vector<std::string> historyCheckpointCandidateTaskIds_;
    std::string historyCheckpointInspectionError_;
    std::jthread historyThread_;
    std::atomic<bool> historyLoading_{false};
    std::uint64_t historyGeneration_ = 0;
    std::vector<restoration::ModelPack> temporalRestorationModels_;
    std::vector<restoration::ModelPack> spatialRestorationModels_;
    std::vector<restoration::ModelPack> deblurRestorationModels_;
    std::vector<restoration::ModelPack> compressionRestorationModels_;
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
    media::ColorInfo sourceColor_;
    bool encoderAvailable_ = false;
    std::vector<media::EncoderCapability> encoderCapabilities_;
    std::vector<media::EncoderCapability> encoderCapabilitiesYuv420p_;
    bool deviceAvailable_ = false;
    std::wstring inspectedSource_;
    std::uint64_t sourceGeneration_ = 0;
    std::uint64_t activeInspectionGeneration_ = 0;
    bool inspectionPending_ = false;
#endif
};

} // namespace rifelove::native_desktop

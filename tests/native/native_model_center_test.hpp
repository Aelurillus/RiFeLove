#pragma once

#include "native_app.hpp"
#include "native_model_store.hpp"
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "application/job_serialization.hpp"
#endif

#include <chrono>
#include <fstream>
#include <span>

namespace rifelove::native_test {

struct ModelCenterTemporaryDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"rifelove-model-center-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    ModelCenterTemporaryDirectory() { std::filesystem::create_directories(root); }
    ~ModelCenterTemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

inline void writeModelCenterText(const std::filesystem::path& file, const std::string& value) {
    std::ofstream output(file, std::ios::binary);
    output << value;
    if (!output) throw std::runtime_error("Cannot write model-center fixture");
}

template<class Check>
void checkLocalModelPreferences(Check check) {
    using namespace native_desktop;
    ModelCenterTemporaryDirectory temporary;
    const auto file = temporary.root / L"data" / L"local-models.txt";
    check(loadNativeModelPreferences(file).registered.empty(), "missing model preferences start empty");
    const auto external = temporary.root / L"外部 自定义模型";
    std::filesystem::create_directory(external);
    writeModelCenterText(external / L"original.onnx", "original bytes");
    NativeModelPreferences preferences;
    preferences.registerDirectory(external);
    preferences.registerDirectory(external / L".");
    preferences.defaults[0] = external;
    preferences.defaults[3] = external;
    preferences.defaults[4] = external;
    saveNativeModelPreferences(file, preferences);
    auto loaded = loadNativeModelPreferences(file);
    check(loaded.registered.size() == 1 && loaded.defaults[0] == external && loaded.defaults[3] == external &&
        loaded.defaults[4] == external,
        "Unicode paths and default models round-trip without duplicate registrations");
    auto oldPreferences = loaded;
    oldPreferences.defaults[4].clear();
    saveNativeModelPreferences(file, oldPreferences);
    const auto legacy = loadNativeModelPreferences(file);
    check(legacy.defaults[3] == external && legacy.defaults[4].empty(),
        "old four-category preferences load with independent deblur still disabled");
    saveNativeModelPreferences(file, loaded);
    loaded.unregisterDirectory(external);
    saveNativeModelPreferences(file, loaded);
    loaded = loadNativeModelPreferences(file);
    check(loaded.registered.empty() && loaded.defaults[0].empty() && loaded.defaults[3].empty() &&
        loaded.defaults[4].empty() && loaded.isExcluded(external),
        "unregister persists a discovery exclusion and clears matching defaults");
    check(std::filesystem::file_size(external / L"original.onnx") == 14,
        "unregister leaves the original model bytes intact");
    loaded.registerDirectory(external);
    check(!loaded.isExcluded(external), "registering an excluded directory restores its application registration");
    bool rejected = false;
    try { (void)checkedNativeModelDirectory(L"\\\\offline-invalid-host\\pack"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "network model directories are rejected before filesystem traversal");
    rejected = false;
    try { (void)nativeModelPath("relative/model"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "stored model paths must be absolute local-drive paths");
    const auto link = temporary.root / L"linked-model";
    if (CreateSymbolicLinkW(link.c_str(), external.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        rejected = false;
        try { (void)checkedNativeModelDirectory(link); } catch (const std::exception&) { rejected = true; }
        check(rejected, "model-directory reparse points cannot bypass local-only checks");
        std::filesystem::remove(link);
    }
    writeModelCenterText(file, "RifeLoveLocalModels 1\ndefault 9 \"C:\\\\invalid\"\n");
    rejected = false;
    try { (void)loadNativeModelPreferences(file); } catch (const std::exception&) { rejected = true; }
    check(rejected, "malformed model preferences fail visibly rather than silently replacing defaults");
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
inline std::string modelCenterHash(const std::string& value) {
    return inference::sha256(std::as_bytes(std::span<const char>(value.data(), value.size())));
}

// These tiny graphs exercise manifest/hash handling only; no ONNX session is
// created and the fixture is never presented as inference qualification.
inline void makeModelCenterPack(const std::filesystem::path& root, int kind, bool supported = true, int scale = 2) {
    std::filesystem::create_directories(root);
    const std::string graph = "abc", license = "MIT fixture";
    const auto id = kind == 0 ? "rife-center-fixture" : kind == 1 ? "spatial-center-fixture" :
        kind == 4 ? "deblur-center-fixture" : "temporal-center-fixture";
    const auto adapter = kind == 0 ? "rife-sdr-v1" : !supported ? "fixture-denoiser-v1" :
        kind == 1 ? "image-sr-rgb-f32-nchw-v1" : kind == 4 ? "nafnet-zoo-deblur-rgb-f32-nchw-v1" : "fastdvdnet-rgb-f32-nchw16-v1";
    const auto numerical = kind == 0 ? "sdr8" : kind == 1 || kind == 4 ? "sdr8-fp32-cpu-reference" : "sdr8-fp32-pytorch-reference";
    const std::string profiles = R"(["bt709-sdr8"])";
    const std::string backends = kind == 0 ? R"(["pytorch_cpu","onnxruntime_cpu"])" :
        kind == 1 || kind == 4 ? R"(["onnxruntime_cpu_source","onnxruntime_cpu_derived"])" :
        R"(["pytorch_cpu_source","onnxruntime_cpu_derived"])";
    const std::string capabilities = kind == 0 ?
        R"({"inputLayout":"NCHW","dtype":"float32","batch":1,"channels":3,"inputRange":[0.0,1.0],"spatialMultiple":64,"timestepShape":[1,1,1,1],"dynamicTimestep":true,"dynamicSpatial":true,"supportedInputProfiles":["bt709-sdr8"],"hdrQualified":false})" :
        std::string(R"({"tasks":[")") + (kind == 1 ? "super-resolution" : kind == 4 ? "deblur" : "denoise") +
        R"("],"placements":[")" + (kind == 1 || kind == 4 ? "output-rate-after-interpolation" : "source-rate-before-interpolation") +
        R"("],"supportedInputProfiles":["bt709-sdr8"],"temporalFramesBefore":)" + (kind == 2 ? "2" : "0") +
        R"(,"temporalFramesAfter":)" + (kind == 2 ? "2" : "0") + R"(,"scaleNumerator":)" + std::to_string(kind == 1 ? scale : 1) +
        R"(,"scaleDenominator":1,"supportsBoundaryPadding":)" + (kind == 2 ? "true}" : "false}");
    const std::string provenance = std::string(R"({"schemaVersion":1,"modelId":")") + id +
        R"(","upstreamRepository":"https://example.invalid/offline-fixture","upstreamCommit":"abcdef0123456789","license":"MIT","licenseScopeEvidence":"https://example.invalid/license"})";
    const std::string adapterJson = std::string(R"({"id":")") + adapter + R"(","version":1})";
    const std::string validation = std::string(R"({"status":"passed_cpu_reference","model_sha256":")") + modelCenterHash(graph) +
        R"(","model_adapter":)" + adapterJson + R"(,"numerical_profile":")" + numerical + R"(","validated_backends":)" + backends +
        (kind == 0 ? R"(,"supported_input_profiles":)" + profiles : R"(,"capabilities":)" + capabilities) + R"(,"cases":[{"passed":true}]})";
    const std::string descriptor = std::string(R"({"schemaVersion":)") + (kind == 0 ? "2" : "1") + R"(,"id":")" + id +
        R"(","displayName":")" + (kind == 0 ? "RIFE Fixture" : kind == 1 ? "SR Fixture" : kind == 4 ? "Deblur Fixture" : "Temporal Fixture") +
        R"(","family":")" + (kind == 0 ? "rife" : "video-restoration") + R"(","version":"1.0","adapter":)" + adapterJson +
        R"(,"modelFile":"model.onnx","sha256":")" + modelCenterHash(graph) + R"(","license":{"id":"MIT","file":"LICENSE.txt","sha256":")" +
        modelCenterHash(license) + R"("},"provenance":{"manifestFile":"source-manifest.json","manifestSha256":")" + modelCenterHash(provenance) +
        R"("},"capabilities":)" + capabilities + R"(,"validation":{"reportFile":"validation.json","reportSha256":")" + modelCenterHash(validation) +
        R"(","numericalProfile":")" + numerical + R"(","validatedBackends":)" + backends + "}}";
    writeModelCenterText(root / L"model.onnx", graph);
    writeModelCenterText(root / L"LICENSE.txt", license);
    writeModelCenterText(root / L"source-manifest.json", provenance);
    writeModelCenterText(root / L"validation.json", validation);
    writeModelCenterText(root / L"model.json", descriptor);
}

template<class Check>
void checkLocalModelCenter(Check check) {
    using namespace native_desktop;
    ModelCenterTemporaryDirectory temporary;
    const auto appRoot = temporary.root / L"app";
    const auto userRoot = temporary.root / L"LocalAppData";
    const auto bundled = appRoot / L"models" / L"rife-bundled";
    const auto rife = temporary.root / L"外部 RIFE";
    const auto spatial = temporary.root / L"外部 SR";
    const auto temporal = temporary.root / L"外部 Temporal";
    const auto deblur = temporary.root / L"外部 Deblur";
    const auto unsupported = temporary.root / L"外部 Unsupported";
    const auto unsupportedDeblur = temporary.root / L"外部 Unsupported Deblur";
    const auto unsupportedScale = temporary.root / L"外部 SR4";
    makeModelCenterPack(bundled, 0);
    makeModelCenterPack(rife, 0);
    makeModelCenterPack(spatial, 1);
    makeModelCenterPack(temporal, 2);
    makeModelCenterPack(deblur, 4);
    makeModelCenterPack(unsupported, 2, false);
    makeModelCenterPack(unsupportedDeblur, 4, false);
    makeModelCenterPack(unsupportedScale, 1, true, 4);
    writeModelCenterText(appRoot / L"models" / L"default-model.txt", "\xEF\xBB\xBFrife-bundled\r\n");
    const auto find = [&](const auto& app, const auto& path) {
        const auto& items = app.testLocalModels();
        for (std::size_t index = 0; index < items.size(); ++index)
            if (NativeModelPreferences::key(items[index].directory) == NativeModelPreferences::key(path)) return index;
        return items.size();
    };
    {
        NativeApp app(appRoot, userRoot);
        check(app.testViewState().modelReady, "verified bundled marker model remains usable");
        check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "create isolated model-center owner");
        const HWND main = app.testMainWindow();
        check(app.testRegisterModel(rife) && app.testRegisterModel(spatial) && app.testRegisterModel(temporal) &&
            app.testRegisterModel(deblur),
            "registers verified local RIFE, super-resolution, temporal and deblur packs");
        check(app.testRegisterModel(unsupported) && app.testRegisterModel(unsupportedDeblur) &&
            app.testRegisterModel(unsupportedScale),
            "unsupported verified packs remain inspectable in model center");
        const auto unsupportedIndex = find(app, unsupported);
        const auto unsupportedDeblurIndex = find(app, unsupportedDeblur);
        const auto scaleIndex = find(app, unsupportedScale);
        check(unsupportedIndex < app.testLocalModels().size() && app.testLocalModels()[unsupportedIndex].verified &&
            !app.testLocalModels()[unsupportedIndex].usable && !app.testSetDefaultModel(unsupportedIndex),
            "verified unknown adapters are never advertised as usable defaults");
        check(scaleIndex < app.testLocalModels().size() && !app.testLocalModels()[scaleIndex].usable && !app.testSetDefaultModel(scaleIndex),
            "unsupported super-resolution scale cannot enter the native x2 selector");
        check(unsupportedDeblurIndex < app.testLocalModels().size() &&
            app.testLocalModels()[unsupportedDeblurIndex].kind == 4 &&
            !app.testLocalModels()[unsupportedDeblurIndex].usable &&
            !app.testSetDefaultModel(unsupportedDeblurIndex),
            "unqualified deblur adapter remains inspectable but cannot become a processing default");
        check(app.testSetDefaultModel(find(app, rife)) && app.testSetDefaultModel(find(app, spatial)) &&
            app.testSetDefaultModel(find(app, temporal)) && app.testSetDefaultModel(find(app, deblur)),
            "each model category has an independent persistent default");
        const HWND center = app.testShowModelCenter();
        check(center != nullptr && GetDlgItem(center, 1201) && GetDlgItem(center, 1202), "model center has a real native list and readable details");
        if (center) {
            const HWND list = GetDlgItem(center, 1201);
            const auto select = [&](const auto& path) {
                SendMessageW(list, LB_SETCURSEL, find(app, path), 0);
                SendMessageW(center, WM_COMMAND, MAKEWPARAM(1201, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
            };
            select(unsupported);
            check(!IsWindowEnabled(GetDlgItem(center, 1204)), "default action is visibly disabled for unsupported models");
            select(rife);
            const HWND details = GetDlgItem(center, 1202);
            std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(details)) + 1, L'\0');
            GetWindowTextW(details, text.data(), static_cast<int>(text.size()));
            check(text.find(rife.wstring()) != std::wstring::npos && text.find(L"bt709-sdr8") != std::wstring::npos &&
                text.find(L"哈希一致") != std::wstring::npos, "details expose full path, color/input profile and actual verification status");
            select(deblur);
            text.assign(static_cast<std::size_t>(GetWindowTextLengthW(details)) + 1, L'\0');
            GetWindowTextW(details, text.data(), static_cast<int>(text.size()));
            check(text.find(L"实验性独立去模糊") != std::wstring::npos && text.find(L"SDR8") != std::wstring::npos &&
                text.find(L"显存") != std::wstring::npos && text.find(L"自然动态曝光") != std::wstring::npos,
                "model center explains deblur qualification, artifact and exposure limitations");
            const UINT dpi = GetDpiForWindow(center);
            SetWindowPos(center, nullptr, 0, 0, MulDiv(500, dpi, 96), MulDiv(540, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{}, listRect{}, detailRect{};
            GetClientRect(center, &client);
            GetWindowRect(list, &listRect); MapWindowPoints(nullptr, center, reinterpret_cast<POINT*>(&listRect), 2);
            GetWindowRect(details, &detailRect); MapWindowPoints(nullptr, center, reinterpret_cast<POINT*>(&detailRect), 2);
            check(listRect.bottom <= detailRect.top && detailRect.bottom < client.bottom,
                "narrow model center stacks list and details without overlap");
            for (int id = 1203; id <= 1207; ++id) {
                RECT rect{}; GetWindowRect(GetDlgItem(center, id), &rect);
                MapWindowPoints(nullptr, center, reinterpret_cast<POINT*>(&rect), 2);
                check(rect.left >= 0 && rect.right <= client.right && rect.top > detailRect.bottom && rect.bottom <= client.bottom,
                    "narrow model-center actions remain inside the window");
            }
            SendMessageW(center, WM_CLOSE, 0, 0);
            check(IsWindow(main), "closing model center keeps the main application alive");
            check(app.testShowModelCenter() != nullptr, "model center can reopen after native control destruction");
        }
        check(SendMessageW(GetDlgItem(main, 1017), CB_GETCURSEL, 0, 0) > 0 &&
            SendMessageW(GetDlgItem(main, 1024), CB_GETCURSEL, 0, 0) > 0,
            "saved restoration defaults are applied to the actual job selectors");
        check(app.testModelPreferences().defaults[4] == deblur &&
            SendMessageW(GetDlgItem(main, 1036), CB_GETCURSEL, 0, 0) == 0 &&
            !app.testExportSettings().automaticDetailRestoration,
            "saved preferred deblur pack never silently enables experimental processing");
        const HWND deblurCombo = GetDlgItem(main, 1036);
        const HWND detailButton = GetDlgItem(main, 1037);
        const HWND detailAdjustment = GetDlgItem(main, 1038);
        const HWND detailReset = GetDlgItem(main, 1039);
        const HWND blur = GetDlgItem(main, 1013);
        check(deblurCombo && detailButton && detailAdjustment && detailReset &&
            SendMessageW(deblurCombo, CB_GETCOUNT, 0, 0) == 2 && !IsWindowEnabled(detailAdjustment),
            "independent deblur selector and zero-default automatic adjustment are visible but off");
        SendMessageW(deblurCombo, CB_SETCURSEL, 1, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1036, CBN_SELCHANGE), reinterpret_cast<LPARAM>(deblurCombo));
        check(app.testViewState().restorationSummary.find(L"去模糊：NAFNet 固定") != std::wstring::npos &&
            app.testViewState().restorationSummary.find(L"超分：×2") != std::wstring::npos &&
            !app.testExportSettings().automaticDetailRestoration,
            "manual deblur and super-resolution remain independently selected with automatic detail off");
        check(app.testSetAutomaticDetail(true) && app.testExportSettings().automaticDetailRestoration &&
            app.testViewState().automaticDetailRestoration && IsWindowEnabled(detailAdjustment),
            "explicit automatic-detail opt-in reaches the real export settings and enables its adjustment");
        SetWindowTextW(detailAdjustment, L"-25.5");
        check(app.testExportSettings().automaticDetailRestoration->relativeAdjustment == -0.255,
            "signed relative detail adjustment is captured without affecting the super-resolution slot");
        application::ProcessingJob dualImageJob;
        dualImageJob.source = "C:/fixture/source.mp4";
        dualImageJob.output = "C:/fixture/output.mkv";
        dualImageJob.modelDirectory = rife;
        dualImageJob.runtimeDirectory = temporary.root / L"runtime";
        check(app.testCaptureSelectedRestoration(dualImageJob) && dualImageJob.deblurModelDirectory == deblur &&
            dualImageJob.restorationModelDirectory == spatial &&
            dualImageJob.expectedDeblurDescriptorSha256 == inference::sha256File(deblur / L"model.json", 256 * 1024) &&
            dualImageJob.expectedRestorationDescriptorSha256 == inference::sha256File(spatial / L"model.json", 256 * 1024),
            "actual selector capture pins independent deblur and super-resolution descriptor hashes");
        dualImageJob.settings = app.testExportSettings();
        const auto restoredDual = application::parseJob(dualImageJob.source, dualImageJob.output,
            application::serializeJob(dualImageJob));
        check(restoredDual.deblurModelDirectory == deblur && restoredDual.restorationModelDirectory == spatial &&
            restoredDual.settings.automaticDetailRestoration &&
            restoredDual.settings.automaticDetailRestoration->relativeAdjustment == -0.255,
            "native dual-slot selection and automatic detail survive queue serialization");
        app.testSetQueueControlsBusy(true);
        check(!IsWindowEnabled(deblurCombo) && !IsWindowEnabled(detailButton) && !IsWindowEnabled(detailAdjustment) &&
            !IsWindowEnabled(detailReset) && !app.testSetAutomaticDetail(false),
            "running queue locks all independent deblur controls without mutating queued settings");
        app.testSetQueueControlsBusy(false);
        SendMessageW(detailReset, BM_CLICK, 0, 0);
        wchar_t detailValue[32]{};
        GetWindowTextW(detailAdjustment, detailValue, static_cast<int>(std::size(detailValue)));
        check(std::wstring(detailValue) == L"0" &&
            app.testExportSettings().automaticDetailRestoration->relativeAdjustment == 0.0,
            "detail reset restores neutral automatic bias");
        SendMessageW(blur, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED), reinterpret_cast<LPARAM>(blur));
        bool exposureRejected = false;
        try { (void)app.testExportSettings(); } catch (const std::invalid_argument&) { exposureRejected = true; }
        check(exposureRejected && app.testViewState().detailAdjustmentSummary.find(L"冲突") != std::wstring::npos,
            "effective natural exposure visibly blocks experimental automatic detail");
        check(app.testSetAutomaticDetail(false) && !app.testSetAutomaticDetail(true),
            "automatic detail cannot be re-enabled while exposure synthesis is active");
        SendMessageW(blur, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1013, BN_CLICKED), reinterpret_cast<LPARAM>(blur));
        check(app.testSetAutomaticDetail(true), "automatic detail can be re-enabled after exposure is disabled");
        SendMessageW(deblurCombo, CB_SETCURSEL, 0, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1036, CBN_SELCHANGE), reinterpret_cast<LPARAM>(deblurCombo));
        GetWindowTextW(detailAdjustment, detailValue, static_cast<int>(std::size(detailValue)));
        check(!app.testExportSettings().automaticDetailRestoration && !app.testViewState().automaticDetailRestoration &&
            std::wstring(detailValue) == L"0" && !IsWindowEnabled(detailAdjustment),
            "turning the deblur model off clears automatic opt-in and its adjustment");
        application::ProcessingJob spatialOnlyJob;
        check(app.testCaptureSelectedRestoration(spatialOnlyJob) && spatialOnlyJob.deblurModelDirectory.empty() &&
            spatialOnlyJob.expectedDeblurDescriptorSha256.empty() && spatialOnlyJob.restorationModelDirectory == spatial,
            "disabling deblur leaves the independent super-resolution job selection intact");
        check(!app.testExportSettings().automaticTemporalDenoise && !app.testViewState().automaticTemporalDenoise,
            "experimental automatic Gaussian denoise defaults off even when a temporal model is selected");
        const HWND adjustment = GetDlgItem(main, 1030), adjustmentReset = GetDlgItem(main, 1031);
        const auto adjustmentText = [&] {
            wchar_t text[64]{}; GetWindowTextW(adjustment, text, static_cast<int>(std::size(text)));
            return std::wstring(text);
        };
        check(adjustment && adjustmentReset && !IsWindowEnabled(adjustment) && !IsWindowEnabled(adjustmentReset) && adjustmentText() == L"0",
            "relative automatic-denoise input and reset default to zero and stay disabled in fixed mode");
        check(app.testExportSettings().temporalDenoiseSigma255 == 25.0F &&
            app.testViewState().restorationSummary.find(L"FastDVDnet 固定 σ25") != std::wstring::npos &&
            app.testViewState().denoiseModeSummary.find(L"仍降噪") != std::wstring::npos &&
            app.testViewState().denoiseAdjustmentSummary.find(L"固定 σ25") != std::wstring::npos,
            "automatic off truthfully identifies selected FastDVDnet as fixed-sigma denoise, not bypass");
        check(app.testSetAutomaticDenoise(true) && app.testExportSettings().automaticTemporalDenoise && app.testViewState().automaticTemporalDenoise,
            "explicit opt-in with a validated FastDVDnet transfers automatic Gaussian denoise into job settings");
        check(IsWindowEnabled(adjustment) && IsWindowEnabled(adjustmentReset) &&
            app.testExportSettings().automaticTemporalDenoise->relativeAdjustment == 0.0,
            "automatic opt-in exposes an independent zero-default relative adjustment");
        for (const auto* value : {L"-100", L"+100", L"-25.5"}) {
            SetWindowTextW(adjustment, value);
            check(app.testExportSettings().automaticTemporalDenoise->relativeAdjustment == native_desktop::nativeRelativeAdjustment(value),
                "visible relative adjustment is captured through the real export settings builder");
        }
        SetWindowTextW(adjustment, L"101");
        bool invalidAdjustmentRejected = false;
        try { (void)app.testExportSettings(); }
        catch (const std::invalid_argument&) { invalidAdjustmentRejected = true; }
        check(invalidAdjustmentRejected && app.testViewState().denoiseAdjustmentSummary.find(L"偏移无效") != std::wstring::npos,
            "out-of-range adjustment blocks task capture and explains the accepted range");
        SendMessageW(adjustmentReset, BM_CLICK, 0, 0);
        check(adjustmentText() == L"0" && app.testExportSettings().automaticTemporalDenoise->relativeAdjustment == 0.0,
            "relative adjustment reset restores the automatic estimate rather than turning denoise off");
        SetWindowTextW(adjustment, L"-25.5");
        app.testSetQueueControlsBusy(true);
        check(!IsWindowEnabled(adjustment) && !IsWindowEnabled(adjustmentReset) && !IsWindowEnabled(GetDlgItem(main, 1026)) &&
            !app.testSetAutomaticDenoise(false) && app.testExportSettings().automaticTemporalDenoise->relativeAdjustment == -0.255,
            "running queue locks the relative input, reset and automatic-mode switch without changing captured values");
        app.testSetQueueControlsBusy(false);
        check(IsWindowEnabled(adjustment) && IsWindowEnabled(adjustmentReset),
            "relative adjustment becomes editable again after the queue releases its controls");
        {
            RECT original{}; GetWindowRect(main, &original);
            const UINT dpi = GetDpiForWindow(main);
            const float scale = static_cast<float>(dpi ? dpi : 96U) / 96.0F;
            const bool priorQuality = app.testViewState().qualityExpanded;
            const HWND disclosure = GetDlgItem(main, 1040);
            check(disclosure != nullptr, "find quality disclosure for automatic-denoise navigation");
            SendMessageW(main, WM_VSCROLL, SB_TOP, 0);
            if (!priorQuality && disclosure) SendMessageW(disclosure, BM_CLICK, 0, 0);
            check(app.testViewState().qualityExpanded, "expand quality settings before navigating automatic denoise");
            for (const int width : {480, 600, 700, 790, 800, 850}) {
                SetWindowPos(main, nullptr, original.left, original.top, MulDiv(width, dpi, 96), MulDiv(600, dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
                SendMessageW(main, WM_VSCROLL, SB_TOP, 0);
                SetFocus(GetDlgItem(main, 1002));
                bool sawInput = false, sawReset = false;
                for (int count = 0; count < 40 && !(sawInput && sawReset); ++count) {
                    app.testNavigateSettings();
                    const HWND focused = GetFocus();
                    if (focused != adjustment && focused != adjustmentReset) continue;
                    RECT client{}, field{}; GetClientRect(main, &client); GetWindowRect(focused, &field);
                    MapWindowPoints(nullptr, main, reinterpret_cast<POINT*>(&field), 2);
                    const auto layout = native_desktop::NativeLayout::from(client.right / scale, client.bottom / scale,
                        app.testViewState().scrollOffset, app.testViewState().qualityExpanded);
                    check(IsWindowVisible(focused) && field.top >= static_cast<LONG>(layout.contentTop * scale) &&
                        field.bottom <= static_cast<LONG>(layout.contentBottom * scale) + 1 &&
                        field.left >= static_cast<LONG>(layout.left * scale) && field.right <= static_cast<LONG>(layout.right * scale) + 1,
                        "Tab scrolls automatic adjustment and reset fully into the viewport at narrow widths");
                    sawInput |= focused == adjustment; sawReset |= focused == adjustmentReset;
                }
                check(sawInput && sawReset, "both independent automatic-denoise controls participate in keyboard navigation");
                check(app.testNavigateSettings(true) && GetFocus() == adjustment,
                    "Shift+Tab returns from reset to the relative input without losing its value");
                check(adjustmentText() == L"-25.5", "responsive layout and keyboard navigation preserve the signed adjustment");
            }
            SetWindowPos(main, nullptr, original.left, original.top, original.right - original.left, original.bottom - original.top, SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(main, WM_VSCROLL, SB_TOP, 0);
            if (!priorQuality && disclosure) SendMessageW(disclosure, BM_CLICK, 0, 0);
        }
        check(app.testViewState().restorationSummary.find(L"自动高斯") != std::wstring::npos &&
            app.testViewState().restorationSummary.find(L"固定 σ25") == std::wstring::npos &&
            app.testViewState().denoiseModeSummary.find(L"软件解码") != std::wstring::npos,
            "automatic opt-in replaces fixed-sigma summary with experimental automatic mode and decode cost");
        check(app.testSetAutomaticDenoise(false) && !app.testExportSettings().automaticTemporalDenoise &&
            app.testViewState().restorationSummary.find(L"固定 σ25") != std::wstring::npos &&
            !IsWindowEnabled(adjustment) && !IsWindowEnabled(adjustmentReset),
            "turning automatic off restores the still-active fixed-sigma mode");
        check(app.testSetAutomaticDenoise(true), "re-enable automatic for the queue serialization fixture");
        application::ProcessingJob denoiseJob;
        denoiseJob.source = "C:/fixture/source.mp4"; denoiseJob.output = "C:/fixture/output.mkv";
        denoiseJob.modelDirectory = rife; denoiseJob.runtimeDirectory = temporary.root / L"runtime";
        denoiseJob.temporalRestorationModelDirectory = temporal;
        denoiseJob.expectedTemporalRestorationDescriptorSha256 = inference::sha256File(temporal / L"model.json", 256 * 1024);
        denoiseJob.settings = app.testExportSettings();
        const auto persisted = application::parseJob(denoiseJob.source, denoiseJob.output, application::serializeJob(denoiseJob));
        check(persisted.settings.automaticTemporalDenoise && persisted.temporalRestorationModelDirectory == temporal &&
            persisted.settings.automaticTemporalDenoise->minimumConfidence == denoiseJob.settings.automaticTemporalDenoise->minimumConfidence &&
            persisted.settings.automaticTemporalDenoise->relativeAdjustment == -0.255,
            "native nonzero relative adjustment and selected temporal model survive the queue serialization boundary");
        SetWindowTextW(adjustment, L"80");
        check(persisted.settings.automaticTemporalDenoise->relativeAdjustment == -0.255 &&
            denoiseJob.settings.automaticTemporalDenoise->relativeAdjustment == -0.255 &&
            app.testExportSettings().automaticTemporalDenoise->relativeAdjustment == 0.8,
            "later UI edits cannot mutate an already-captured or persisted queue task");
        const HWND temporalCombo = GetDlgItem(main, 1024);
        const auto temporalSelection = SendMessageW(temporalCombo, CB_GETCURSEL, 0, 0);
        SendMessageW(temporalCombo, CB_SETCURSEL, 0, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporalCombo));
        check(!app.testExportSettings().automaticTemporalDenoise && !app.testSetAutomaticDenoise(true) &&
            adjustmentText() == L"0" && !IsWindowEnabled(adjustment) && !IsWindowEnabled(adjustmentReset),
            "removing the FastDVDnet selection disables automatic denoise and blocks invalid re-enablement");
        check(app.testViewState().restorationSummary.find(L"降噪：关闭") != std::wstring::npos &&
            app.testViewState().denoiseModeSummary.find(L"可选降噪：关闭") != std::wstring::npos,
            "no temporal model is truthfully labeled as denoise off");
        SendMessageW(temporalCombo, CB_SETCURSEL, temporalSelection, 0);
        SendMessageW(main, WM_COMMAND, MAKEWPARAM(1024, CBN_SELCHANGE), reinterpret_cast<LPARAM>(temporalCombo));
        check(!app.testExportSettings().automaticTemporalDenoise && adjustmentText() == L"0", "reselecting a model cannot silently re-enable experimental processing or restore an old bias");
        if (main) DestroyWindow(main);
    }
    {
        NativeApp app(appRoot, userRoot);
        check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "recreate model-center owner after restart");
        const HWND main = app.testMainWindow();
        check(app.testModelPreferences().defaults[0] == rife && app.testModelPreferences().defaults[1] == spatial &&
            app.testModelPreferences().defaults[2] == temporal && app.testModelPreferences().defaults[4] == deblur &&
            app.testViewState().modelReady,
            "RIFE, super-resolution, denoise and deblur defaults survive a new application instance");
        check(SendMessageW(GetDlgItem(main, 1017), CB_GETCURSEL, 0, 0) > 0 &&
            SendMessageW(GetDlgItem(main, 1024), CB_GETCURSEL, 0, 0) > 0 &&
            SendMessageW(GetDlgItem(main, 1036), CB_GETCURSEL, 0, 0) == 0 &&
            !app.testExportSettings().automaticDetailRestoration,
            "restoration defaults survive restart while experimental deblur remains off");
        writeModelCenterText(rife / L"LICENSE.txt", "tampered");
        app.testRefreshModels();
        const auto corrupt = find(app, rife);
        check(corrupt < app.testLocalModels().size() && !app.testLocalModels()[corrupt].usable && !app.testViewState().modelReady,
            "changed default model sidecars block readiness without silently choosing another RIFE");
        check(!app.testRegisterModel(rife), "tampered packs cannot be registered as verified local models");
        check(app.testUnregisterModel(find(app, spatial)), "unregisters a registered external model");
        check(std::filesystem::exists(spatial / L"model.onnx") && app.testModelPreferences().defaults[1].empty() &&
            SendMessageW(GetDlgItem(main, 1017), CB_GETCURSEL, 0, 0) == 0,
            "unregister clears actual selection and default but preserves external files");
        check(app.testUnregisterModel(find(app, bundled)), "bundled models can be excluded from application discovery");
        check(std::filesystem::exists(bundled / L"model.onnx"), "bundled model originals also survive unregister");
        if (main) DestroyWindow(main);
    }
    {
        NativeApp app(appRoot, userRoot);
        check(find(app, bundled) == app.testLocalModels().size() && find(app, spatial) == app.testLocalModels().size(),
            "unregistered bundled and external directories stay unregistered after restart");
        check(app.testRegisterModel(bundled), "explicit registration restores an excluded bundled model");
    }
    // Each temporary main window posts its normal WM_QUIT. This test drives
    // several application lifetimes without NativeApp::run(), so consume only
    // those quit messages before the remaining controls/history tests start.
    MSG quit{};
    while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
}
#endif
} // namespace rifelove::native_test

#pragma once

#include "native_app.hpp"
#include "native_model_center_test.hpp"
#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
#include "restoration/stdf_luma_adapter.hpp"
#endif

namespace rifelove::native_test {

template<class Check>
void checkNativeCompressionContract(Check check) {
    using namespace native_desktop;
    check(nativeCompressionStrength(0) == 0.0F && nativeCompressionStrength(1) == 0.25F &&
        nativeCompressionStrength(2) == 0.5F && nativeCompressionStrength(3) == 1.0F,
        "compression switch and all strength presets map to independent exact job values");
    for (const int selection : {-1, 4}) {
        bool rejected = false;
        try { (void)nativeCompressionStrength(selection); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "unknown compression strengths cannot silently enable a job");
    }
    media::ColorInfo color;
    color.pixelFormat = "yuv420p"; color.bitDepth = 8;
    color.primaries = color.transfer = color.matrix = "bt709"; color.range = "tv";
    check(nativeCompressionSourceIssue(384, 256, color).empty(), "compression readiness accepts the qualified SDR8 boundary");
    check(!nativeCompressionSourceIssue(385, 256, color).empty() && !nativeCompressionSourceIssue(384, 257, color).empty() &&
        !nativeCompressionSourceIssue(0, 256, color).empty(), "compression readiness refuses unqualified dimensions before starting");
    check(nativeCompressionSourceIssue(1920, 1080, color, true).empty() &&
        nativeCompressionSourceIssue(1918, 1078, color, true).empty() &&
        nativeCompressionSourceIssue(854, 480, color, true).empty() &&
        nativeCompressionSourceIssue(3840, 2160, color, true).empty() &&
        nativeCompressionSourceIssue(2160, 3840, color, true).empty(),
        "explicit tiled mode accepts landscape and portrait UHD within its area budget");
    check(!nativeCompressionSourceIssue(3840, 3840, color, true).empty() &&
        !nativeCompressionSourceIssue(4096, 2160, color, true).empty() &&
        !nativeCompressionSourceIssue(1920, 0, color, true).empty(),
        "tiled readiness checks both maximum axis and total pixels");
    for (int change = 0; change < 10; ++change) {
        auto invalid = color;
        switch (change) {
        case 0: invalid.pixelFormat = "nv12"; break;
        case 1: invalid.bitDepth = 10; break;
        case 2: invalid.primaries = "bt470bg"; break;
        case 3: invalid.transfer = "smpte170m"; break;
        case 4: invalid.matrix = "unknown"; break;
        case 5: invalid.range = "pc"; break;
        case 6: invalid.hdrTransfer = true; break;
        case 7: invalid.masteringDisplay.emplace(); break;
        case 8: invalid.contentLight = media::ContentLight{1000, 400}; break;
        case 9: invalid.transfer.clear(); break;
        }
        check(!nativeCompressionSourceIssue(320, 180, invalid).empty(),
            "compression UI gate mirrors strict service color/pixel-format and HDR metadata rules");
        check(!nativeCompressionSourceIssue(1920, 1080, invalid, true).empty(),
            "explicit tiling cannot bypass color, bit-depth or HDR contracts");
    }
}

#ifdef RIFELOVE_NATIVE_HAS_PROCESSING
template<class Check>
void checkNativeCompressionWorkflow(Check check) {
    using namespace native_desktop;
    ModelCenterTemporaryDirectory temporary;
    const auto packDirectory = temporary.root / L"descriptor-only STDF";
    std::filesystem::create_directory(packDirectory);
    writeModelCenterText(packDirectory / L"model.json", "descriptor capture fixture; never loaded for inference");
    restoration::ModelPack pack;
    pack.directory = packDirectory; pack.id = "stdf-descriptor-fixture"; pack.displayName = "STDF descriptor fixture";
    pack.graphSha256 = validation::stdf::graphSha256;
    pack.numericalProfile = validation::stdf::numericalProfile;
    pack.metadataIntegrityVerified = true;
    pack.validatedBackends = {validation::stdf::cpuValidationBackend, validation::stdf::cudaValidationBackend};
    pack.capabilities.modelId = pack.id; pack.capabilities.adapterId = validation::stdf::adapterId;
    pack.capabilities.adapterVersion = validation::stdf::adapterVersion;
    pack.capabilities.tasks = {restoration::Task::deblock};
    pack.capabilities.placements = {restoration::Placement::sourceRateBeforeInterpolation};
    pack.capabilities.supportedInputProfiles = {"bt709-sdr8"};
    pack.capabilities.temporalFramesBefore = pack.capabilities.temporalFramesAfter = 3;
    pack.capabilities.scaleNumerator = pack.capabilities.scaleDenominator = 1;
    pack.capabilities.supportsBoundaryPadding = true;
    NativeApp app(temporary.root / L"app", temporary.root / L"profile");
    check(app.create(GetModuleHandleW(nullptr), SW_SHOWNOACTIVATE), "create isolated native compression workflow");
    const HWND window = app.testMainWindow();
    if (!window) return;
    const HWND model = GetDlgItem(window, 1027), strength = GetDlgItem(window, 1028), tiled = GetDlgItem(window, 1029);
    check(model && strength && tiled && SendMessageW(strength, CB_GETCOUNT, 0, 0) == 4 &&
        SendMessageW(strength, CB_GETCURSEL, 0, 0) == 0 && SendMessageW(tiled, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
        "native compression has separate model, default-off strength and explicit default-off tiling controls");
    app.testSetCompressionModels({pack});
    SendMessageW(model, CB_SETCURSEL, 1, 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1027, CBN_SELCHANGE), reinterpret_cast<LPARAM>(model));
    check(IsWindowEnabled(strength) && SendMessageW(strength, CB_GETCURSEL, 0, 0) == 0,
        "selecting a qualified STDF pack alone cannot enable compression restoration");
    application::ProcessingJob job;
    job.source = "C:/fixture/source.mp4"; job.output = "C:/fixture/output.mkv";
    job.modelDirectory = temporary.root / L"rife"; job.runtimeDirectory = temporary.root / L"runtime";
    for (int selection = 1; selection <= 3; ++selection) {
        SendMessageW(strength, CB_SETCURSEL, selection, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1028, CBN_SELCHANGE), reinterpret_cast<LPARAM>(strength));
        check(app.testCaptureSelectedRestoration(job) && job.compressionRestorationModelDirectory == packDirectory &&
            job.compressionRestorationStrength == nativeCompressionStrength(selection) &&
            job.expectedCompressionRestorationDescriptorSha256 == inference::sha256File(packDirectory / L"model.json", 256 * 1024) &&
            job.temporalRestorationModelDirectory.empty() && job.restorationModelDirectory.empty(),
            "native compression selection captures its own pinned model and strength without substituting denoise or SR");
        const auto stored = application::parseJob(job.source, job.output, application::serializeJob(job));
        check(stored.compressionRestorationModelDirectory == packDirectory && stored.compressionRestorationStrength == job.compressionRestorationStrength &&
            stored.expectedCompressionRestorationDescriptorSha256 == job.expectedCompressionRestorationDescriptorSha256 &&
            !stored.compressionRestorationTiled,
            "native compression identity and strength survive the queue schema boundary");
    }
    SendMessageW(tiled, BM_CLICK, 0, 0);
    check(SendMessageW(tiled, BM_GETCHECK, 0, 0) == BST_CHECKED && app.testCaptureSelectedRestoration(job) &&
        job.compressionRestorationTiled &&
        application::parseJob(job.source, job.output, application::serializeJob(job)).compressionRestorationTiled,
        "explicit native tiling opt-in survives schema serialization independently of strength");
    auto temporal = pack, spatial = pack;
    temporal.directory = temporary.root / L"temporal"; spatial.directory = temporary.root / L"spatial";
    std::filesystem::create_directory(temporal.directory); std::filesystem::create_directory(spatial.directory);
    writeModelCenterText(temporal.directory / L"model.json", "temporal descriptor");
    writeModelCenterText(spatial.directory / L"model.json", "spatial descriptor");
    check(app.testCaptureRestorationDescriptors(job, &temporal, &spatial, &pack, 0.5F) &&
        job.temporalRestorationModelDirectory == temporal.directory && job.restorationModelDirectory == spatial.directory &&
        job.compressionRestorationModelDirectory == packDirectory, "three independent restoration stages coexist in the captured job");
    const auto originalHash = job.expectedCompressionRestorationDescriptorSha256;
    std::filesystem::remove(packDirectory / L"model.json");
    check(!app.testCaptureRestorationDescriptors(job, &temporal, &spatial, &pack, 1.0F) &&
        job.expectedCompressionRestorationDescriptorSha256 == originalHash && job.compressionRestorationStrength == 0.5F,
        "missing STDF descriptor produces a recoverable startup failure without partially changing the captured job");
    auto unsupported = pack; unsupported.metadataIntegrityVerified = false;
    check(!app.testCaptureRestorationDescriptors(job, nullptr, nullptr, &unsupported, 0.5F),
        "unqualified STDF packs cannot enter the job even through a stale native selection");
    app.testSetCompressionModels({});
    check(SendMessageW(strength, CB_GETCURSEL, 0, 0) == 0 && !IsWindowEnabled(strength) &&
        SendMessageW(tiled, BM_GETCHECK, 0, 0) == BST_UNCHECKED && !IsWindowEnabled(tiled),
        "removing a compression pack disables and resets its opt-in");
    app.testSetCompressionModels({pack});
    check(SendMessageW(strength, CB_GETCURSEL, 0, 0) == 0,
        "model refresh cannot silently restore experimental compression strength");
    check(app.testCaptureRestorationDescriptors(job, nullptr, nullptr) && job.compressionRestorationModelDirectory.empty() &&
        job.expectedCompressionRestorationDescriptorSha256.empty() && job.compressionRestorationStrength == 0.0F &&
        !job.compressionRestorationTiled,
        "turning compression off clears its complete job identity");
    DestroyWindow(window);
    MSG quit{}; while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
}
#endif
} // namespace rifelove::native_test

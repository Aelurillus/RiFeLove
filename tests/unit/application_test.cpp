#include "application/job_serialization.hpp"
#include "application/local_lease.hpp"
#include "application/import_service.hpp"
#include "application/task_queue.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <random>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../integration/platform_import_test.hpp"

TEST(BatchPlatformImport, PersistsPerSourcePortraitLandscapeFractionalRateAndSuperResolutionPolicy) {
    rifelove::tests::platform_import::verifyPersistedPolicy();
}

TEST(BatchImportDimensions, ValidatesEachSourceAgainstTargetAndModelScale) {
    using rifelove::application::supportsImportOutputDimensions;
    EXPECT_TRUE(supportsImportOutputDimensions(1920, 1080, 0, 0, 2, 1));
    EXPECT_TRUE(supportsImportOutputDimensions(1920, 1080, 2560, 1440, 2, 1));
    EXPECT_FALSE(supportsImportOutputDimensions(1920, 1080, 3840, 2160, 1, 1));
    EXPECT_FALSE(supportsImportOutputDimensions(1080, 1920, 2560, 1440, 2, 1));
    EXPECT_FALSE(supportsImportOutputDimensions(1920, 1080, 2560, 0, 2, 1));
    EXPECT_FALSE(supportsImportOutputDimensions(0, 1080, 0, 0, 2, 1));
    EXPECT_FALSE(supportsImportOutputDimensions(1920, 1080, 2560, 1440, 0, 1));
}

TEST(BatchPlatformBitrate, UsesEachSourceHeightAndTargetRate) {
    using namespace rifelove::application;
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 720, 30), 5000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 720, 60), 7500);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 1080, 30), 8000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 1080, 60), 12000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 1440, 30), 16000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 2160, 30), 45000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 4320, 30), 160000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 4320, 60), 240000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, 1080, 300), 12000);
    EXPECT_EQ(platformGopSizeFrames(PlatformBitrate::youtube, 60), 30);
    EXPECT_EQ(platformGopSizeFrames(PlatformBitrate::youtube, 30), 15);
    EXPECT_EQ(platformGopSizeFrames(PlatformBitrate::localQuality, 60), 120);
    EXPECT_THROW(platformGopSizeFrames(PlatformBitrate::unchanged, 60), std::invalid_argument);
    EXPECT_THROW(platformGopSizeFrames(PlatformBitrate::youtube, 0), std::invalid_argument);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::localQuality, 1080, 60), 20000);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::localQuality, 1080, 120), 40000);
    EXPECT_EQ(platformOutputHeight(1080, 0, 2, 1), 2160);
    EXPECT_EQ(platformOutputHeight(1080, 1440, 2, 1), 1440);
    EXPECT_EQ(platformBitrateKbps(PlatformBitrate::youtube, platformOutputHeight(1080, 0, 2, 1), 60), 68000);
    EXPECT_THROW(platformBitrateKbps(PlatformBitrate::youtube, 0, 60), std::invalid_argument);
    EXPECT_THROW(platformBitrateKbps(PlatformBitrate::youtube, 1080, 0), std::invalid_argument);
    EXPECT_THROW(platformOutputHeight(1080, 0, 2, 0), std::invalid_argument);
}

TEST(BatchPlatformBitrate, RoundsRationalRatesUpWithoutOverflowOrFallback) {
    using namespace rifelove;
    using namespace rifelove::application;
    EXPECT_EQ(platformFrameRateCeiling(Rational(30000,1001)),30);
    EXPECT_EQ(platformFrameRateCeiling(Rational(60000,1001)),60);
    EXPECT_EQ(platformFrameRateCeiling(Rational(60001,1000)),61);
    EXPECT_EQ(platformFrameRateCeiling(Rational(1,std::numeric_limits<std::int64_t>::max())),1);
    EXPECT_EQ(platformFrameRateCeiling(Rational(std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::max() - 1)),2);
    EXPECT_EQ(platformFrameRateCeiling(Rational(std::numeric_limits<int>::max())),std::numeric_limits<int>::max());
    EXPECT_THROW(platformFrameRateCeiling(Rational(0)),std::invalid_argument);
    EXPECT_THROW(platformFrameRateCeiling(Rational(-1)),std::invalid_argument);
    EXPECT_THROW(platformFrameRateCeiling(Rational(static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1)),std::invalid_argument);
    EXPECT_THROW(platformGopSizeFrames(PlatformBitrate::localQuality,std::numeric_limits<int>::max()),std::invalid_argument);
    EXPECT_EQ(platformGopSizeFrames(PlatformBitrate::youtube,std::numeric_limits<int>::max()),1073741824);
}

TEST(BatchPlatformBitrate, UsesActualShortEdgeForPortraitLandscapeScaleAndExplicitTargets) {
    using namespace rifelove;
    using namespace rifelove::application;
    for (const auto rate : {Rational(30000,1001),Rational(60000,1001)}) {
        const bool high = rate > Rational(30);
        for (const auto profile : {PlatformBitrate::youtube,PlatformBitrate::localQuality}) {
            const auto landscape = platformExportSettings(profile,rate,1920,1080);
            const auto portrait = platformExportSettings(profile,rate,1080,1920);
            EXPECT_EQ(landscape.bitRateKbps,profile == PlatformBitrate::youtube ? (high ? 12000 : 8000) : 20000);
            EXPECT_EQ(portrait.bitRateKbps,landscape.bitRateKbps);
            EXPECT_EQ(portrait.gopSizeFrames,profile == PlatformBitrate::youtube ? (high ? 30 : 15) : (high ? 120 : 60));
            const auto scaled = platformExportSettings(profile,rate,1080,1920,0,0,2,1);
            EXPECT_EQ(scaled.outputWidth,2160); EXPECT_EQ(scaled.outputHeight,3840);
            EXPECT_EQ(scaled.bitRateKbps,profile == PlatformBitrate::youtube ? (high ? 68000 : 45000) : 70000);
            const auto explicitTarget = platformExportSettings(profile,rate,1080,1920,1440,2560,2,1);
            EXPECT_EQ(explicitTarget.outputWidth,1440); EXPECT_EQ(explicitTarget.outputHeight,2560);
            EXPECT_EQ(explicitTarget.bitRateKbps,profile == PlatformBitrate::youtube ? (high ? 24000 : 16000) : 40000);
        }
    }
    EXPECT_THROW(platformExportSettings(PlatformBitrate::youtube,Rational(60),1920,1080,0,1440),std::invalid_argument);
    EXPECT_THROW(platformExportSettings(PlatformBitrate::youtube,Rational(60),1920,1080,2560,0),std::invalid_argument);
    EXPECT_THROW(platformExportSettings(PlatformBitrate::youtube,Rational(60),0,1080),std::invalid_argument);
    EXPECT_THROW(platformExportSettings(PlatformBitrate::youtube,Rational(60),1920,1081,0,0,1,2),std::invalid_argument);
    EXPECT_THROW(platformExportSettings(PlatformBitrate::youtube,Rational(60),std::numeric_limits<int>::max(),1080,0,0,2,1),std::invalid_argument);
    EXPECT_THROW(platformExportSettings(PlatformBitrate::unchanged,Rational(60),1920,1080),std::invalid_argument);
}

namespace {
using namespace rifelove;
using namespace rifelove::application;
ProcessingJob configuredJob() {
    ProcessingJob job; job.source = "C:/recordings/source.mkv"; job.output = "C:/exports/result.mkv";
    job.modelDirectory = L"C:/models/local"; job.runtimeDirectory = L"C:/runtime";
    job.settings.targetRate = Rational(60000,1001); job.deviceOrdinal = 1;
    job.sourceDurationEstimate = Rational(1001,24000);
    job.settings.hostFloatBufferLimit = 256 * 1024 * 1024;
    job.settings.inferenceBackend = "cuda";
    job.settings.temporalDenoiseSigma255 = 17.5F;
    job.temporalRestorationModelDirectory = L"C:/local/denoiser";
    job.expectedTemporalRestorationDescriptorSha256 = std::string(64,'c');
    job.settings.motionBlur.enabled = true;
    job.settings.motionBlur.shutterAngleDegrees = 180;
    job.settings.motionBlur.sampleCount = 4;
    job.expectedModelId = "rife-4.25"; job.expectedModelVersion = "4.25"; job.expectedModelSha256 = std::string(64,'a'); job.expectedSourceSha256 = std::string(64,'b');
    auto& e = job.settings.encoding; e.encoder = "hevc_nvenc"; e.pixelFormat = "p010le"; e.videoStreamIndex = 2;
    e.outputWidth = 2560; e.outputHeight = 1440;
    e.timeBase = Rational(1,120000); e.frameRate = job.settings.targetRate; e.bitRate = 12345678; e.gopSize = 48; e.maxBFrames = 3;
    e.encoderOptions = {{"cq","20"},{"preset","p5"}};
    job.settings.scene = {0.2,0.3,0.6,1e-4}; return job;
}
std::optional<std::filesystem::path> repositoryPath(const std::filesystem::path& relative) {
    std::vector<std::filesystem::path> roots{std::filesystem::current_path()};
    const auto source = std::filesystem::path(__FILE__);
    roots.push_back(source.is_absolute() ? source.parent_path() : std::filesystem::absolute(source).parent_path());
    for (auto root : roots) {
        while (!root.empty()) {
            const auto candidate = root / relative;
            if (std::filesystem::is_regular_file(candidate / "model.json")) return std::filesystem::weakly_canonical(candidate);
            const auto parent = root.parent_path();
            if (parent == root) break;
            root = parent;
        }
    }
    return std::nullopt;
}
bool writeTaggedImportSource(const std::filesystem::path& path, const std::string& colorSettings) {
    const auto range = colorSettings.find("range=full") != std::string::npos ? "pc" : "tv";
    const auto transfer = colorSettings.find("iec61966-2-1") != std::string::npos ? "iec61966-2-1" : "bt709";
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=256x144:rate=2:duration=0.5\" -vf \"format=yuv420p,setparams=" +
        colorSettings + "\" -color_range " + range + " -color_primaries bt709 -color_trc " + transfer +
        " -colorspace bt709 -c:v ffv1 -frames:v 1 \"" + path.string() + "\"";
    return std::system(command.c_str()) == 0;
}
bool writeDualTaggedImportSource(const std::filesystem::path& path) {
    const auto command = "ffmpeg.exe -y -v error -f lavfi -i \"testsrc2=size=256x144:rate=2:duration=0.5\" -f lavfi -i \"testsrc2=size=256x144:rate=2:duration=0.5\" "
        "-map 0:v -map 1:v -filter:v:0 \"format=yuv420p,setparams=range=tv:color_primaries=bt709:color_trc=bt709:colorspace=bt709\" "
        "-filter:v:1 \"format=yuv420p,setparams=range=full:color_primaries=bt709:color_trc=bt709:colorspace=bt709\" "
        "-color_range:v:0 tv -color_range:v:1 pc -color_primaries:v:0 bt709 -color_primaries:v:1 bt709 "
        "-color_trc:v:0 bt709 -color_trc:v:1 bt709 -colorspace:v:0 bt709 -colorspace:v:1 bt709 -c:v ffv1 -frames:v 1 \"" + path.string() + "\"";
    return std::system(command.c_str()) == 0;
}
}
TEST(BatchImport, CompressionUsesExplicitVideoStreamAndExactStdfColorContract) {
    const auto rife = repositoryPath("models/cache/rife-4.25-fp32-sdr8-v3");
    const auto stdf = repositoryPath("models/cache/stdf-r3-y7-sdr8-local-eval");
    if (!rife || !stdf) GTEST_SKIP() << "local RIFE/STDF evaluation model packs are not available";
    rifelove::tests::platform_import::Fixture fixture;
    const auto tagged = fixture.directory / "tagged.mkv";
    rifelove::tests::platform_import::writeSource(tagged,256,144);

    auto makeRequest = [&](const std::filesystem::path& source, int streamIndex) {
        rifelove::application::ImportRequest request;
        request.inputs = {source}; request.outputDirectory = fixture.directory;
        request.settings.modelDirectory = *rife;
        request.settings.runtimeDirectory = fixture.directory / "runtime";
        request.settings.compressionRestorationModelDirectory = *stdf;
        request.settings.compressionRestorationStrength = 0.75F;
        request.settings.settings.targetRate = rifelove::Rational(60);
        request.settings.settings.encoding.videoStreamIndex = streamIndex;
        return request;
    };

    {
        rifelove::application::TaskQueue queue(fixture.directory / "invalid-stream.sqlite");
        const auto imported = rifelove::application::importVideos(queue,makeRequest(tagged,42));
        ASSERT_TRUE(imported.taskIds.empty());
        ASSERT_EQ(imported.rejected.size(),1U);
        EXPECT_EQ(imported.rejected.front().reason,rifelove::application::ImportRejection::noVideo);
    }

    const auto dual = fixture.directory / "dual-profile.mkv";
    ASSERT_TRUE(writeDualTaggedImportSource(dual));
    const auto dualInfo = rifelove::media::MediaEngine::probe(dual.string());
    ASSERT_EQ(dualInfo.streams.size(),2U);
    ASSERT_EQ(dualInfo.streams[0].color.range,"tv");
    ASSERT_EQ(dualInfo.streams[1].color.range,"pc");
    {
        rifelove::application::TaskQueue queue(fixture.directory / "explicit-profile-stream.sqlite");
        const auto imported = rifelove::application::importVideos(queue,makeRequest(dual,1));
        EXPECT_TRUE(imported.taskIds.empty());
        ASSERT_EQ(imported.rejected.size(),1U);
        EXPECT_EQ(imported.rejected.front().reason,rifelove::application::ImportRejection::unsupportedRestorationModelProfile);
    }

    for (const auto& [name, colorSettings] : std::array<std::pair<const char*,const char*>,2>{
             std::pair{"full-range", "range=full:color_primaries=bt709:color_trc=bt709:colorspace=bt709"},
             std::pair{"srgb-transfer", "range=tv:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709"}}) {
        const auto source = fixture.directory / (std::string(name) + ".mkv");
        ASSERT_TRUE(writeTaggedImportSource(source,colorSettings));
        const auto info = rifelove::media::MediaEngine::probe(source.string());
        const auto selected = std::find_if(info.streams.begin(),info.streams.end(),[&](const auto& stream) {
            return stream.index == info.preferredVideoStream && stream.type == "video";
        });
        ASSERT_NE(selected,info.streams.end());
        if (std::string(name) == "full-range") EXPECT_EQ(selected->color.range,"pc");
        else EXPECT_EQ(selected->color.transfer,"iec61966-2-1");
        rifelove::application::TaskQueue queue(fixture.directory / (std::string(name) + ".sqlite"));
        const auto imported = rifelove::application::importVideos(queue,makeRequest(source,-1));
        EXPECT_TRUE(imported.taskIds.empty());
        ASSERT_EQ(imported.rejected.size(),1U);
        EXPECT_EQ(imported.rejected.front().reason,rifelove::application::ImportRejection::unsupportedRestorationModelProfile);
    }
}
TEST(ProcessingConfiguration, RoundTripsAllAdvancedSettingsAndPinnedIdentity) {
    const auto job = configuredJob(); const auto encoded = serializeJob(job);
    const auto restored = parseJob(job.source,job.output,encoded);
    EXPECT_EQ(restored.source,job.source); EXPECT_EQ(restored.output,job.output);
    EXPECT_EQ(serializeJob(restored),encoded);
    EXPECT_EQ(restored.settings.targetRate,Rational(60000,1001)); EXPECT_EQ(restored.settings.encoding.timeBase,Rational(1,120000));
    EXPECT_EQ(restored.settings.inferenceBackend,"cuda");
    EXPECT_FLOAT_EQ(restored.settings.temporalDenoiseSigma255,17.5F);
    EXPECT_EQ(restored.temporalRestorationModelDirectory,job.temporalRestorationModelDirectory);
    EXPECT_EQ(restored.expectedTemporalRestorationDescriptorSha256,job.expectedTemporalRestorationDescriptorSha256);
    EXPECT_TRUE(restored.settings.motionBlur.enabled);
    EXPECT_EQ(nlohmann::json::parse(encoded).at("motionBlur").at("algorithmVersion"),2);
    EXPECT_EQ(restored.settings.motionBlur.shutterAngleDegrees,180);
    EXPECT_EQ(restored.settings.motionBlur.sampleCount,4);
    EXPECT_EQ(restored.sourceDurationEstimate,job.sourceDurationEstimate);
    EXPECT_EQ(restored.settings.encoding.outputWidth,2560);
    EXPECT_EQ(restored.settings.encoding.outputHeight,1440);
    EXPECT_EQ(restored.settings.encoding.encoderOptions.at("preset"),"p5"); EXPECT_EQ(restored.settings.encoding.maxBFrames,3);
}

TEST(AutomaticDenoiseConfiguration, PersistsAlgorithmReplayPolicyAndIndependentParameters) {
    auto job = configuredJob();
    job.settings.automaticTemporalDenoise = rifelove::restoration::AutomaticDenoiseSettings{-0.25,0.7,12.0,0.8};
    const auto encoded = serializeJob(job);
    const auto document = nlohmann::json::parse(encoded);
    EXPECT_EQ(document.at("schemaVersion"),12);
    EXPECT_EQ(document.at("automaticTemporalDenoise").at("algorithmVersion"),1);
    EXPECT_EQ(document.at("automaticTemporalDenoise").at("replayPolicy"),"software-source-pts-from-start-v1");
    const auto restored = parseJob(job.source,job.output,encoded);
    ASSERT_TRUE(restored.settings.automaticTemporalDenoise);
    EXPECT_DOUBLE_EQ(restored.settings.automaticTemporalDenoise->relativeAdjustment,-0.25);
    EXPECT_DOUBLE_EQ(restored.settings.automaticTemporalDenoise->smoothingSeconds,0.7);
    EXPECT_DOUBLE_EQ(restored.settings.automaticTemporalDenoise->maximumSigmaChangePerSecond,12.0);
    EXPECT_DOUBLE_EQ(restored.settings.automaticTemporalDenoise->minimumConfidence,0.8);
    EXPECT_EQ(serializeJob(restored),encoded);
    for (const auto* field : {"algorithmVersion","replayPolicy","relativeAdjustment","smoothingSeconds","maximumSigmaChangePerSecond","minimumConfidence"}) {
        auto bad = document; bad["automaticTemporalDenoise"].erase(field);
        EXPECT_ANY_THROW((void)parseJob(job.source,job.output,bad.dump()));
    }
    auto bad = document; bad["automaticTemporalDenoise"]["algorithmVersion"] = 2;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["automaticTemporalDenoise"]["replayPolicy"] = "seek-to-last-frame";
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["automaticTemporalDenoise"]["relativeAdjustment"] = 1.1;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    auto legacy = nlohmann::json::parse(serializeJob(configuredJob()));
    legacy["schemaVersion"] = 8; legacy.erase("automaticTemporalDenoise");
    const auto fixed = parseJob(job.source,job.output,legacy.dump());
    EXPECT_FALSE(fixed.settings.automaticTemporalDenoise);
    EXPECT_FLOAT_EQ(fixed.settings.temporalDenoiseSigma255,17.5F);
}

TEST(AutomaticCompressionConfiguration, IsExplicitOptInAndPinsEveryAnalyzerAndReplayParameter) {
    auto job = configuredJob();
    const auto disabled = nlohmann::json::parse(serializeJob(job));
    EXPECT_TRUE(disabled.at("automaticCompressionRestoration").is_null());
    job.settings.automaticCompressionRestoration.emplace();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.75F;
    job.settings.automaticCompressionRestoration = rifelove::restoration::AutomaticCompressionSettings{-0.25,0.7,0.4,0.8};
    const auto encoded = serializeJob(job);
    const auto document = nlohmann::json::parse(encoded);
    EXPECT_EQ(document.at("schemaVersion"),12);
    const auto& automatic = document.at("automaticCompressionRestoration");
    EXPECT_EQ(automatic.at("algorithmVersion"),2);
    EXPECT_EQ(automatic.at("analyzer"),"sdr8-luma-block8-stratified-v2");
    EXPECT_EQ(automatic.at("replayPolicy"),"software-source-pts-from-start-v1");
    const auto restored = parseJob(job.source,job.output,encoded);
    ASSERT_TRUE(restored.settings.automaticCompressionRestoration);
    EXPECT_DOUBLE_EQ(restored.settings.automaticCompressionRestoration->relativeAdjustment,-0.25);
    EXPECT_DOUBLE_EQ(restored.settings.automaticCompressionRestoration->smoothingSeconds,0.7);
    EXPECT_DOUBLE_EQ(restored.settings.automaticCompressionRestoration->maximumStrengthChangePerSecond,0.4);
    EXPECT_DOUBLE_EQ(restored.settings.automaticCompressionRestoration->minimumConfidence,0.8);
    EXPECT_EQ(serializeJob(restored),encoded);
    for (const auto* field : {"algorithmVersion","analyzer","replayPolicy","relativeAdjustment","smoothingSeconds","maximumStrengthChangePerSecond","minimumConfidence"}) {
        auto bad = document; bad["automaticCompressionRestoration"].erase(field);
        EXPECT_ANY_THROW((void)parseJob(job.source,job.output,bad.dump())) << field;
    }
    auto bad = document; bad["automaticCompressionRestoration"]["algorithmVersion"] = 1;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document;
    bad["automaticCompressionRestoration"]["algorithmVersion"] = 1;
    bad["automaticCompressionRestoration"]["analyzer"] = "sdr8-luma-block8-excess-v1";
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["automaticCompressionRestoration"]["analyzer"] = "unvalidated-codec-detector";
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["automaticCompressionRestoration"]["replayPolicy"] = "seek";
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["automaticCompressionRestoration"]["relativeAdjustment"] = 1.1;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["schemaVersion"] = 11;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad.erase("automaticCompressionRestoration");
    EXPECT_ANY_THROW((void)parseJob(job.source,job.output,bad.dump()));
    bad = document; bad["schemaVersion"] = 11; bad.erase("automaticCompressionRestoration");
    EXPECT_FALSE(parseJob(job.source,job.output,bad.dump()).settings.automaticCompressionRestoration);
}
TEST(ProcessingConfiguration, CompressionRestorationIsIndependentAndLegacySchemasDefaultFullFrame) {
    auto job = configuredJob();
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.75F;
    const auto encoded = serializeJob(job);
    const auto document = nlohmann::json::parse(encoded);
    EXPECT_EQ(document.at("schemaVersion"),12);
    EXPECT_EQ(document.at("compressionRestorationModelDirectory"),"C:/models/stdf-r3");
    EXPECT_EQ(document.at("expectedCompressionRestorationDescriptorSha256"),std::string(64,'d'));
    EXPECT_FLOAT_EQ(document.at("compressionRestorationStrength").get<float>(),0.75F);
    EXPECT_FALSE(document.at("compressionRestorationTiled").get<bool>());
    EXPECT_EQ(document.at("compressionRestorationPolicy").at("mode"),"full-frame");
    const auto restored = parseJob(job.source,job.output,encoded);
    EXPECT_EQ(restored.compressionRestorationModelDirectory,job.compressionRestorationModelDirectory);
    EXPECT_EQ(restored.expectedCompressionRestorationDescriptorSha256,job.expectedCompressionRestorationDescriptorSha256);
    EXPECT_FLOAT_EQ(restored.compressionRestorationStrength,0.75F);
    EXPECT_FALSE(restored.compressionRestorationTiled);

    auto schemaTen = document;
    schemaTen["schemaVersion"] = 10;
    schemaTen.erase("compressionRestorationTiled");
    schemaTen.erase("compressionRestorationPolicy");
    const auto legacyEnabled = parseJob(job.source,job.output,schemaTen.dump());
    EXPECT_FALSE(legacyEnabled.compressionRestorationTiled);
    EXPECT_EQ(legacyEnabled.compressionRestorationModelDirectory,job.compressionRestorationModelDirectory);

    auto legacy = document;
    legacy["schemaVersion"] = 9;
    legacy.erase("compressionRestorationModelDirectory");
    legacy.erase("expectedCompressionRestorationDescriptorSha256");
    legacy.erase("compressionRestorationStrength");
    const auto disabled = parseJob(job.source,job.output,legacy.dump());
    EXPECT_TRUE(disabled.compressionRestorationModelDirectory.empty());
    EXPECT_TRUE(disabled.expectedCompressionRestorationDescriptorSha256.empty());
    EXPECT_FLOAT_EQ(disabled.compressionRestorationStrength,0.0F);
    EXPECT_FALSE(disabled.compressionRestorationTiled);
}
TEST(ProcessingConfiguration, SchemaElevenAlwaysRecordsAnExplicitCompressionExecutionPolicy) {
    auto job = configuredJob();
    const auto disabled = nlohmann::json::parse(serializeJob(job));
    EXPECT_EQ(disabled.at("schemaVersion"),12);
    ASSERT_TRUE(disabled.at("compressionRestorationTiled").is_boolean());
    EXPECT_FALSE(disabled.at("compressionRestorationTiled").get<bool>());
    EXPECT_EQ(disabled.at("compressionRestorationPolicy").dump(),serializeCompressionRestorationPolicy(false));

    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.75F;
    job.compressionRestorationTiled = true;
    const auto encoded = serializeJob(job);
    const auto document = nlohmann::json::parse(encoded);
    EXPECT_TRUE(document.at("compressionRestorationTiled").get<bool>());
    EXPECT_EQ(document.at("compressionRestorationPolicy").dump(),serializeCompressionRestorationPolicy(true));
    const auto& policy = document.at("compressionRestorationPolicy");
    EXPECT_EQ(policy.at("mode"),"tiled");
    EXPECT_EQ(policy.at("coreWidth"),256);
    EXPECT_EQ(policy.at("coreHeight"),128);
    EXPECT_EQ(policy.at("halo"),64);
    EXPECT_EQ(policy.at("alignment"),8);
    EXPECT_EQ(policy.at("maximumInputWidth"),384);
    EXPECT_EQ(policy.at("maximumInputHeight"),256);
    EXPECT_EQ(policy.at("edgePolicy"),"global-right-bottom-pad-to8-clip-halo-v1");
    EXPECT_EQ(policy.at("maximumAxis"),3840);
    EXPECT_EQ(policy.at("maximumPixels"),3840U * 2160U);
    const auto restored = parseJob(job.source,job.output,encoded);
    EXPECT_TRUE(restored.compressionRestorationTiled);
    EXPECT_EQ(serializeJob(restored),encoded);

    auto bad = document;
    bad.erase("compressionRestorationTiled");
    EXPECT_ANY_THROW((void)parseJob(job.source,job.output,bad.dump()));
    for (const auto& flag : {nlohmann::json(1),nlohmann::json("true"),nlohmann::json(nullptr)}) {
        bad = document; bad["compressionRestorationTiled"] = flag;
        EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    }
    bad = document; bad.erase("compressionRestorationPolicy");
    EXPECT_ANY_THROW((void)parseJob(job.source,job.output,bad.dump()));
    for (const auto& [field,value] : policy.items()) {
        bad = document; bad["compressionRestorationPolicy"].erase(field);
        EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument) << field;
        bad = document; bad["compressionRestorationPolicy"][field] = "changed";
        EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument) << field;
    }
    bad = document; bad["compressionRestorationPolicy"]["alignment"] = 8.0;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["compressionRestorationPolicy"]["extra"] = true;
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
    bad = document; bad["compressionRestorationPolicy"] = nlohmann::json::parse(serializeCompressionRestorationPolicy(false));
    EXPECT_THROW((void)parseJob(job.source,job.output,bad.dump()),std::invalid_argument);
}
TEST(ProcessingConfiguration, TiledCompressionCannotBeEnabledWithoutTheCompleteModelBundle) {
    auto job = configuredJob();
    job.temporalRestorationModelDirectory.clear();
    job.expectedTemporalRestorationDescriptorSha256.clear();
    job.compressionRestorationTiled = true;
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    EXPECT_THROW(pinRestorationModelDescriptor(job),std::invalid_argument);
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = 0.75F;
    EXPECT_NO_THROW((void)serializeJob(job));
    job.expectedCompressionRestorationDescriptorSha256.clear();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationModelDirectory.clear();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
}
TEST(ProcessingConfiguration, LegacySchemasDefaultFullFrameAndRejectSmuggledTiledPolicies) {
    const auto job = configuredJob();
    for (int version = 1; version <= 10; ++version) {
        auto legacy = nlohmann::json::parse(serializeJob(job));
        legacy["schemaVersion"] = version;
        legacy.erase("compressionRestorationTiled");
        legacy.erase("compressionRestorationPolicy");
        EXPECT_FALSE(parseJob(job.source,job.output,legacy.dump()).compressionRestorationTiled) << version;
        legacy["compressionRestorationTiled"] = true;
        EXPECT_THROW((void)parseJob(job.source,job.output,legacy.dump()),std::invalid_argument) << version;
        legacy["compressionRestorationTiled"] = false;
        legacy["compressionRestorationPolicy"] = nlohmann::json::parse(serializeCompressionRestorationPolicy(true));
        EXPECT_THROW((void)parseJob(job.source,job.output,legacy.dump()),std::invalid_argument) << version;
    }
}
TEST(ProcessingConfiguration, CompressionDimensionPolicyKeepsLegacyBoundsAndExplicitBounded4kTiling) {
    EXPECT_TRUE(supportsCompressionRestorationDimensions(false,384,256));
    EXPECT_TRUE(supportsCompressionRestorationDimensions(false,383,255));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(false,385,256));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(false,384,257));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(false,1920,1080));
    EXPECT_TRUE(supportsCompressionRestorationDimensions(true,1920,1080));
    EXPECT_TRUE(supportsCompressionRestorationDimensions(true,3840,2160));
    EXPECT_TRUE(supportsCompressionRestorationDimensions(true,2160,3840));
    EXPECT_TRUE(supportsCompressionRestorationDimensions(true,1919,1079));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(true,3840,3840));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(true,3841,2160));
    EXPECT_FALSE(supportsCompressionRestorationDimensions(true,2160,3841));
    for (const bool tiled : {false,true}) {
        EXPECT_FALSE(supportsCompressionRestorationDimensions(tiled,0,1));
        EXPECT_FALSE(supportsCompressionRestorationDimensions(tiled,1,0));
        EXPECT_FALSE(supportsCompressionRestorationDimensions(tiled,-1,1));
        EXPECT_FALSE(supportsCompressionRestorationDimensions(tiled,1,-1));
        EXPECT_FALSE(supportsCompressionRestorationDimensions(tiled,std::numeric_limits<int>::max(),std::numeric_limits<int>::max()));
    }
}
TEST(ProcessingConfiguration, CompressionRestorationStrengthAndBundleAreStrictlyValidated) {
    auto job = configuredJob();
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.0F;
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = -0.01F;
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = 1.01F;
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = std::numeric_limits<float>::infinity();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.compressionRestorationStrength = 0.5F;
    job.expectedCompressionRestorationDescriptorSha256.clear();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationModelDirectory.clear();
    EXPECT_THROW((void)serializeJob(job),std::invalid_argument);
}
TEST(ProcessingConfiguration, RejectsNarrowingMalformedHashesAndInvalidThresholds) {
    const auto job = configuredJob(); auto json = nlohmann::json::parse(serializeJob(job));
    json.erase("sourceDurationEstimate");
    EXPECT_NO_THROW((void)parseJob(job.source,job.output,json.dump()));
    json = nlohmann::json::parse(serializeJob(job));
    json["deviceOrdinal"] = 4294967296ULL;
    EXPECT_THROW((void)parseJob(job.source,job.output,json.dump()),std::invalid_argument);
    json = nlohmann::json::parse(serializeJob(job)); json["encoding"]["bitRate"] = 18446744073709551615ULL;
    EXPECT_THROW((void)parseJob(job.source,job.output,json.dump()),std::invalid_argument);
    json = nlohmann::json::parse(serializeJob(job)); json["encoding"]["maxBFrames"] = 1.5;
    EXPECT_THROW((void)parseJob(job.source,job.output,json.dump()),std::invalid_argument);
    json = nlohmann::json::parse(serializeJob(job)); json["motionBlur"]["algorithmVersion"] = 999;
    EXPECT_THROW((void)parseJob(job.source,job.output,json.dump()),std::invalid_argument);
    auto bad = job; bad.expectedSourceSha256 = "unverified"; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.scene.cutMeanDifference = -0.1; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.inferenceBackend = "fp16"; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.motionBlur.shutterAngleDegrees = 361; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.motionBlur.sampleCount = 17; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.temporalDenoiseSigma255 = 4.9F; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.temporalDenoiseSigma255 = 55.1F; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.automaticTemporalDenoise.emplace(); bad.settings.automaticTemporalDenoise->minimumConfidence = -0.1;
    EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.expectedTemporalRestorationDescriptorSha256.clear(); EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.temporalRestorationModelDirectory.clear(); EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.encoding.frameRate = Rational(24); EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.encoding.outputWidth = 2560; bad.settings.encoding.outputHeight = 0; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.encoding.outputWidth = 16385; bad.settings.encoding.outputHeight = 9216; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.expectedModelAdapterId = "bad/id"; bad.expectedModelAdapterVersion = 1;
    EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.expectedModelAdapterVersion = 1;
    EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
}
TEST(ProcessingConfiguration, ReadsLegacyVersionOneJobsWithoutAnAdapterPin) {
    const auto job = configuredJob();
    auto legacy = nlohmann::json::parse(serializeJob(job));
    legacy["schemaVersion"] = 1;
    legacy.erase("expectedModelAdapterId");
    legacy.erase("expectedModelAdapterVersion");
    legacy.erase("inferenceBackend");
    legacy.erase("motionBlur");
    const auto restored = parseJob(job.source,job.output,legacy.dump());
    EXPECT_TRUE(restored.expectedModelAdapterId.empty());
    EXPECT_EQ(restored.expectedModelAdapterVersion,0);
    EXPECT_FALSE(restored.settings.motionBlur.enabled);
    EXPECT_FLOAT_EQ(restored.settings.temporalDenoiseSigma255,25.0F);
}
TEST(ProcessingConfiguration, ReadsLegacyVersionTwoJobsWithAutomaticBackendDefault) {
    const auto job = configuredJob();
    auto legacy = nlohmann::json::parse(serializeJob(job));
    legacy["schemaVersion"] = 2;
    legacy.erase("inferenceBackend");
    const auto restored = parseJob(job.source,job.output,legacy.dump());
    EXPECT_EQ(restored.settings.inferenceBackend,"auto");
    auto schemaSix = nlohmann::json::parse(serializeJob(job));
    schemaSix["schemaVersion"] = 6;
    schemaSix.erase("temporalDenoiseSigma255");
    const auto restoredSix = parseJob(job.source,job.output,schemaSix.dump());
    EXPECT_FLOAT_EQ(restoredSix.settings.temporalDenoiseSigma255,25.0F);
    auto schemaSeven = nlohmann::json::parse(serializeJob(job));
    schemaSeven["schemaVersion"] = 7;
    schemaSeven.erase("temporalRestorationModelDirectory");
    schemaSeven.erase("expectedTemporalRestorationDescriptorSha256");
    const auto restoredSeven = parseJob(job.source,job.output,schemaSeven.dump());
    EXPECT_TRUE(restoredSeven.temporalRestorationModelDirectory.empty());
    EXPECT_TRUE(restoredSeven.expectedTemporalRestorationDescriptorSha256.empty());
}
TEST(ProcessingConfiguration, PinsVersionTwoModelDescriptorForPersistentJobs) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("rifelove-model-descriptor-v2-" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    const std::string digest(64,'c');
    const nlohmann::json descriptor{{"schemaVersion",2},{"id","fixture-v2"},{"family","rife"},
        {"version","4.25"},{"sha256",digest},{"capabilities",nlohmann::json::object()},
        {"license",nlohmann::json::object()},{"provenance",nlohmann::json::object()},{"validation",nlohmann::json::object()}};
    { std::ofstream file(directory / "model.json",std::ios::binary); file << descriptor.dump(); }
    auto job = configuredJob(); job.modelDirectory = directory;
    job.expectedModelSha256.clear(); job.expectedModelId.clear(); job.expectedModelVersion.clear();
    ASSERT_NO_THROW(pinModelDescriptor(job));
    EXPECT_EQ(job.expectedModelId,"fixture-v2");
    EXPECT_EQ(job.expectedModelVersion,"4.25");
    EXPECT_EQ(job.expectedModelSha256,digest);
    EXPECT_EQ(job.expectedModelAdapterId,"rife-sdr-v1");
    EXPECT_EQ(job.expectedModelAdapterVersion,1);

    const auto customDescriptor = nlohmann::json{{"schemaVersion",2},{"id","fixture-v2"},{"family","rife"},
        {"version","4.25"},{"sha256",digest},{"adapter",{{"id","fixture-adapter"},{"version",3}}},
        {"capabilities",nlohmann::json::object()},{"license",nlohmann::json::object()},{"provenance",nlohmann::json::object()},
        {"validation",nlohmann::json::object()}};
    { std::ofstream file(directory / "model.json",std::ios::binary | std::ios::trunc); file << customDescriptor.dump(); }
    job.expectedModelAdapterId.clear(); job.expectedModelAdapterVersion = 0;
    ASSERT_NO_THROW(pinModelDescriptor(job));
    EXPECT_EQ(job.expectedModelAdapterId,"fixture-adapter");
    EXPECT_EQ(job.expectedModelAdapterVersion,3);
    const auto roundTrip = parseJob(job.source,job.output,serializeJob(job));
    EXPECT_EQ(roundTrip.expectedModelAdapterId,"fixture-adapter");
    EXPECT_EQ(roundTrip.expectedModelAdapterVersion,3);
}
TEST(ProcessingIdentity, StoresAdapterVersionAndUpgradesLegacyIdentityToBuiltin) {
    ProcessingIdentity identity;
    identity.source.sha256 = std::string(64,'b');
    identity.modelId = "fixture-rife";
    identity.modelVersion = "4.25";
    identity.modelSha256 = std::string(64,'a');
    identity.backend = "onnxruntime_cuda";
    identity.adapterId = "fixture-adapter";
    identity.adapterVersion = 3;
    const auto encoded = serializeIdentity(identity);
    const auto json = nlohmann::json::parse(encoded);
    ASSERT_EQ(json.at("schemaVersion"),2);
    EXPECT_EQ(json.at("model").at("adapter").at("id"),"fixture-adapter");
    EXPECT_EQ(json.at("model").at("adapter").at("version"),3);

    ProcessingJob restored;
    applyStoredIdentity(restored,encoded);
    EXPECT_EQ(restored.expectedModelAdapterId,"fixture-adapter");
    EXPECT_EQ(restored.expectedModelAdapterVersion,3);
    auto conflicting = restored;
    conflicting.expectedModelAdapterId = "rife-sdr-v1";
    conflicting.expectedModelAdapterVersion = 1;
    EXPECT_THROW(applyStoredIdentity(conflicting,encoded),std::invalid_argument);

    auto legacy = json;
    legacy["schemaVersion"] = 1;
    legacy["model"].erase("adapter");
    ProcessingJob legacyJob;
    applyStoredIdentity(legacyJob,legacy.dump());
    EXPECT_EQ(legacyJob.expectedModelAdapterId,"rife-sdr-v1");
    EXPECT_EQ(legacyJob.expectedModelAdapterVersion,1);
}
TEST(ProcessingReport, SerializesRuntimeIdentityQualityDiagnosticsAndTimingEvidence) {
    PipelineResult result{};
    result.video.encoder = "libopenh264"; result.video.framesSubmitted = 120; result.video.videoPackets = 120; result.video.copiedPackets = 48;
    result.video.firstTimestamp = Rational(3,2); result.video.endExclusive = Rational(7,2);
    result.sourceFrames = 60; result.aiFrames = 60; result.processedFrames = 120; result.reusedFrames = 0;
    result.inferenceCalls = 240; result.originalFrames = 60; result.heldFrames = 0;
    result.motionBlurActive = true; result.motionBlurredFrames = 117; result.motionBlurFallbackFrames = 3;
    result.motionBlurFallbackReasons = {{"scene_cut_hold",3}};
    result.motionBlurEdgeHeldFrames = 2; result.motionBlurPeakWindowFrames = 12;
    result.motionBlurPeakPreparedBytes = 12 * 64 * 64 * 3 * sizeof(float);
    result.sceneCuts = 1; result.duplicatePairs = 2; result.abnormalPairs = 3; result.recoveredTimestamps = 4;
    result.modelId = "rife-4.25-fp32-sdr8"; result.backend = "onnxruntime_tensorrt_cuda";
    result.restorationModelId = "realesrgan-x2"; result.restorationBackend = "onnxruntime_cuda";
    result.restorationFallbackReason = "TensorRT provider unavailable";
    result.temporalRestorationModelId = "fastdvdnet"; result.temporalRestorationBackend = "onnxruntime_tensorrt_cuda";
    result.inferenceFallbackReason.clear(); result.encoderFallbackReason = "NVENC API unavailable"; result.sourceEndEvidence = "declared_stream_end";
    result.elapsedSeconds = 10.0; result.processingFps = 12.0;
    result.timings.sourceDecodeSeconds = 1.25; result.timings.inferenceWaitSeconds = 4.5;
    result.timings.frameEncodeSeconds = 0.75; result.timings.outputValidationSeconds = 0.5; result.timings.pipelineWallSeconds = 9.0;
    result.validation.decodedFrames = 120; result.validation.timeBase = Rational(1,120);
    result.validation.maximumTimestampError = Rational(1,240); result.validation.durationError = Rational(0);
    result.validation.declaredEndError = Rational(0); result.validation.auxiliaryMaximumEndError = Rational(0); result.validation.quantized = true;
    ProcessingReportContext context;
    context.sourcePath = "C:/输入/源片.mkv"; context.outputPath = "C:/输出/结果.mp4"; context.targetFrameRate = "60";
    context.modelVersion = "4.25"; context.modelSha256 = std::string(64,'a'); context.endToEndElapsedSeconds = 12.0;
    context.device = {0,"NVIDIA GeForce GTX 1660 SUPER","00000000:01:00.0",7,5,12020,6'442'450'944};
    context.encoding.encoder = "auto"; context.encoding.bitRate = 8'000'000; context.encoding.gopSize = 120; context.encoding.maxBFrames = 0;
    context.encoding.encoderOptions = {{"preset","p5"}};

    const auto report = nlohmann::json::parse(serializeProcessingReport(context,result));
    EXPECT_EQ(report.at("schemaVersion"),1);
    EXPECT_EQ(report.at("source").at("path"),context.sourcePath);
    EXPECT_EQ(report.at("output").at("targetFrameRate"),"60");
    EXPECT_EQ(report.at("output").at("encoder"),"libopenh264");
    EXPECT_EQ(report.at("model").at("version"),"4.25");
    EXPECT_EQ(report.at("model").at("sha256"),std::string(64,'a'));
    EXPECT_EQ(report.at("gpu").at("name"),"NVIDIA GeForce GTX 1660 SUPER");
    EXPECT_EQ(report.at("encoding").at("requestedEncoder"),"auto");
    EXPECT_EQ(report.at("encoding").at("fallbackReason"),"NVENC API unavailable");
    EXPECT_EQ(report.at("performance").at("endToEndOutputFramesPerSecond"),10.0);
    EXPECT_EQ(report.at("performance").at("pipelineOutputFramesPerSecond"),12.0);
    EXPECT_EQ(report.at("performance").at("processedFrames"),120);
    EXPECT_EQ(report.at("performance").at("reusedFrames"),0);
    EXPECT_EQ(report.at("performance").at("pipelineProcessedFramesPerSecond"),12.0);
    EXPECT_EQ(report.at("performance").at("stageTimings").at("inferenceWaitSeconds"),4.5);
    EXPECT_EQ(report.at("performance").at("stageTimings").at("frameEncodeSeconds"),0.75);
    EXPECT_EQ(report.at("qualityDiagnostics").at("sceneCuts"),1);
    EXPECT_EQ(report.at("qualityDiagnostics").at("perceptualMetricsMeasured"),false);
    EXPECT_EQ(report.at("source").at("recoveredTimestamps"),4);
    EXPECT_EQ(report.at("timingValidation").at("timeBase"),"1/120");
    EXPECT_EQ(report.at("pipelineResult").at("abnormalPairs"),3);
    EXPECT_EQ(report.at("pipelineResult").at("restorationBackend"),"onnxruntime_cuda");
    EXPECT_EQ(report.at("pipelineResult").at("restorationFallbackReason"),"TensorRT provider unavailable");
    EXPECT_EQ(report.at("temporalRestoration").at("id"),"fastdvdnet");
    EXPECT_EQ(report.at("pipelineResult").at("temporalRestorationBackend"),"onnxruntime_tensorrt_cuda");
    EXPECT_EQ(report.at("performance").at("inferenceCalls"),240);
    EXPECT_EQ(report.at("qualityDiagnostics").at("motionBlur").at("blurredFrames"),117);
    EXPECT_EQ(report.at("qualityDiagnostics").at("motionBlur").at("fallbackFrames"),3);
    EXPECT_EQ(report.at("qualityDiagnostics").at("motionBlur").at("fallbackReasons").at("scene_cut_hold"),3);
    EXPECT_EQ(report.at("qualityDiagnostics").at("motionBlur").at("countsScope"),"current_execution");
    EXPECT_EQ(report.at("pipelineResult").at("motionBlur").at("edgeHeldFrames"),2);
    EXPECT_EQ(report.at("pipelineResult").at("timings").at("sourceDecodeSeconds"),1.25);
    EXPECT_EQ(report.at("pipelineResult").at("recoveredTimestamps"),4);
}
TEST(ProcessingMetrics, ResumeSpeedCountsOnlyCurrentFramesAndHandlesZeroAndOverflow) {
    using namespace rifelove;
    EXPECT_DOUBLE_EQ(processingFrameRate(12,6.0),2.0);
    EXPECT_DOUBLE_EQ(processingFrameRate(0,6.0),0.0);
    EXPECT_DOUBLE_EQ(processingFrameRate(12,0.0),0.0);
    EXPECT_DOUBLE_EQ(processingFrameRate(12,-1.0),0.0);
    EXPECT_TRUE(std::isfinite(processingFrameRate(std::numeric_limits<std::size_t>::max(),std::numeric_limits<double>::denorm_min())));
    EXPECT_DOUBLE_EQ(processingFrameRate(std::numeric_limits<std::size_t>::max(),std::numeric_limits<double>::denorm_min()),
        std::numeric_limits<double>::max());

    application::ProcessingEvent progress;
    progress.outputFrames = 120; progress.processedFrames = 12; progress.reusedFrames = 108; progress.processingFps = 2.0;
    const auto progressJson = nlohmann::json::parse(application::serializeProgress(progress));
    EXPECT_EQ(progressJson.at("frames"),120);
    EXPECT_EQ(progressJson.at("processedFrames"),12);
    EXPECT_EQ(progressJson.at("reusedFrames"),108);

    PipelineResult result{};
    result.video.framesSubmitted = 120; result.processedFrames = 0; result.reusedFrames = 120;
    result.elapsedSeconds = 4.0; result.processingFps = 999.0;
    const auto resultJson = nlohmann::json::parse(application::serializeResult(result));
    EXPECT_EQ(resultJson.at("outputFrames"),120);
    EXPECT_EQ(resultJson.at("processedFrames"),0);
    EXPECT_EQ(resultJson.at("reusedFrames"),120);
    EXPECT_EQ(resultJson.at("processingFps"),0.0);
}
TEST(ProcessingReport, PublishesUnicodeJsonWithoutReplacingExistingFiles) {
    const auto directory = std::filesystem::temp_directory_path() / ("rifelove-report-" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    const auto destination = directory / "处理报告.json";
    {
        ProcessingReportFile report(destination);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_NO_THROW(report.publish("{\"schemaVersion\":1}\n"));
        EXPECT_THROW(report.publish("{}"),std::logic_error);
    }
    std::ifstream input(destination,std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    EXPECT_EQ(contents,"{\"schemaVersion\":1}\n");
    EXPECT_THROW((ProcessingReportFile(destination)),std::invalid_argument);
}
TEST(SourceLease, HashesWithBoundedReadsAndPreventsConcurrentWrites) {
    const auto file = std::filesystem::temp_directory_path() / ("rifelove-lease-" + std::to_string(std::random_device{}()) + ".bin");
    const auto created = CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    ASSERT_NE(created,INVALID_HANDLE_VALUE);
    DWORD written = 0; const bool wrote = WriteFile(created,"abc",3,&written,nullptr) != FALSE; CloseHandle(created);
    EXPECT_TRUE(wrote); EXPECT_EQ(written,3U);
    try {
        {
            SourceLease lease(file); const auto identity = lease.fingerprint({});
            EXPECT_EQ(identity.size,3U); EXPECT_EQ(identity.sha256,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
            const auto handle = CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            EXPECT_EQ(handle,INVALID_HANDLE_VALUE); if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            std::stop_source stop; stop.request_stop(); EXPECT_THROW((void)lease.fingerprint(stop.get_token()),std::runtime_error);
        }
        EXPECT_TRUE(std::filesystem::remove(file));
    } catch (...) { std::filesystem::remove(file); throw; }
}

TEST(TaskQueue, FailedTerminalWriteDoesNotStrandAnActiveTaskUntilRestart) {
    const auto directory = std::filesystem::temp_directory_path() / ("rifelove-queue-abort-" + std::to_string(std::random_device{}()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{directory};
    const auto database = directory / "queue.sqlite";
    std::vector<std::string> ids;
    {
        TaskStore seed(database.string());
        // Invalid configuration fails before any media/model/GPU operation.
        ids = seed.createConfigured({{"source-a","out-a","{}"},{"source-b","out-b","{}"}});
    }
    sqlite3* injected = nullptr;
    ASSERT_EQ(sqlite3_open(database.string().c_str(),&injected),SQLITE_OK);
    const auto status = sqlite3_exec(injected,
        "CREATE TRIGGER reject_terminal_write BEFORE UPDATE ON tasks WHEN NEW.state IN ('completed','failed','cancelled') "
        "BEGIN SELECT RAISE(ABORT,'injected terminal write failure'); END",nullptr,nullptr,nullptr);
    sqlite3_close(injected);
    ASSERT_EQ(status,SQLITE_OK);

    TaskQueue queue(database);
    bool drained = false;
    std::string queueError;
    ASSERT_TRUE(queue.start([&](QueueEvent event) {
        if (event.drained) { drained = true; queueError = std::move(event.queueError); }
    }));
    queue.wait();
    EXPECT_TRUE(drained);
    EXPECT_FALSE(queue.busy());
    EXPECT_NE(queueError.find("injected terminal write failure"),std::string::npos);
    const auto aborted = queue.list();
    ASSERT_EQ(aborted.size(),2U);
    EXPECT_EQ(aborted[0].task.id,ids[0]);
    EXPECT_EQ(aborted[0].task.state,TaskState::interrupted);
    EXPECT_NE(aborted[0].task.error.find("terminal state was saved"),std::string::npos);
    EXPECT_EQ(aborted[1].task.state,TaskState::queued);

    ASSERT_EQ(sqlite3_open(database.string().c_str(),&injected),SQLITE_OK);
    const auto dropStatus = sqlite3_exec(injected,"DROP TRIGGER reject_terminal_write",nullptr,nullptr,nullptr);
    sqlite3_close(injected);
    ASSERT_EQ(dropStatus,SQLITE_OK);
    drained = false;
    ASSERT_TRUE(queue.start([&](QueueEvent event) {
        if (event.drained) { drained = true; queueError = std::move(event.queueError); }
    }));
    queue.wait();
    EXPECT_TRUE(drained);
    EXPECT_TRUE(queueError.empty());
    const auto continued = queue.list();
    ASSERT_EQ(continued.size(),2U);
    EXPECT_EQ(continued[0].task.state,TaskState::interrupted);
    EXPECT_EQ(continued[1].task.state,TaskState::failed);
}

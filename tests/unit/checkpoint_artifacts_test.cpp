#include "application/checkpoint_artifacts.hpp"
#include "application/job_serialization.hpp"
#include "application/task_queue.hpp"
#include "inference/model_pack.hpp"
#include "media/video_writer.hpp"
#include "media/ffmpeg_helpers.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/stdf_tile_policy.hpp"
extern "C" {
#include <libswscale/swscale.h>
}

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
void resealProcessingProof(nlohmann::json& pipeline) {
    std::string canonical = "algorithm=" + pipeline.at("algorithmVersion").get<std::string>() + "\n";
    for (const auto& [name,hash] : pipeline.at("sourceFiles").items()) canonical += name + "=" + hash.get<std::string>() + "\n";
    pipeline["sourceSha256"] = rifelove::inference::sha256(std::as_bytes(std::span(canonical)));
}

class CheckpointFixture final {
public:
    CheckpointFixture() {
        std::random_device random;
        directory = std::filesystem::temp_directory_path() /
            ("rifelove-checkpoint-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("checkpoint test directory collision");
        source = directory / "source.ppm";
        std::ofstream file(source,std::ios::binary);
        file << "P6\n2 2\n255\n";
        const std::array<unsigned char,12> pixels{255,0,0,0,255,0,0,0,255,255,255,255};
        file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
        if (!file) throw std::runtime_error("could not create checkpoint video source");
    }
    ~CheckpointFixture() {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
    }
    std::filesystem::path directory;
    std::filesystem::path source;
};

rifelove::media::VideoSegmentInput createSegment(const CheckpointFixture& fixture,
    const rifelove::application::CheckpointArtifacts& artifacts) {
    rifelove::media::DecodedFrame frame{};
    (void)rifelove::media::MediaEngine::decodeVideo(fixture.source.string(),[&](const auto& decoded) { frame = decoded; });
    rifelove::media::VideoEncodingSettings settings;
    settings.encoder = "ffv1";
    settings.pixelFormat = "bgr0";
    settings.timeBase = rifelove::Rational(1,50);
    settings.frameRate = rifelove::Rational(50);
    settings.copyAuxiliaryStreams = false;
    settings.requireFirstPacketKeyframe = true;
    const auto path = artifacts.segmentPath(0);
    rifelove::media::VideoMuxWriter writer(fixture.source.string(),path.string(),settings);
    writer.writeFrame(frame,rifelove::Rational(0),rifelove::Rational(1,50));
    writer.writeFrame(frame,rifelove::Rational(1,50),rifelove::Rational(1,50));
    (void)writer.finish();
    rifelove::media::VideoSegmentContract contract;
    contract.frameCount = 2;
    contract.firstTimestamp = rifelove::Rational(0);
    contract.frameDuration = rifelove::Rational(1,50);
    contract.width = 2;
    contract.height = 2;
    contract.codec = "ffv1";
    contract.encoder = "ffv1";
    contract.pixelFormat = "bgr0";
    contract.bitDepth = 8;
    return {path.string(),contract};
}

std::optional<std::filesystem::path> findLocalStdfPack() {
    std::vector<std::filesystem::path> roots;
    roots.push_back(std::filesystem::current_path());
    const auto sourceFile = std::filesystem::path(__FILE__);
    roots.push_back(sourceFile.is_absolute() ? sourceFile.parent_path() : std::filesystem::absolute(sourceFile).parent_path());
    for (const auto& initial : roots) {
        auto root = initial;
        while (!root.empty()) {
            const auto candidate = root / "models" / "cache" / "stdf-r3-y7-sdr8-local-eval";
            if (std::filesystem::is_regular_file(candidate / "model.json"))
                return std::filesystem::weakly_canonical(candidate);
            const auto parent = root.parent_path();
            if (parent == root) break;
            root = parent;
        }
    }
    return std::nullopt;
}

nlohmann::json compressionManifest(bool tiled) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    ProcessingJob job;
    job.source = "C:/source.mkv"; job.output = "C:/output.mkv";
    job.modelDirectory = "C:/model"; job.runtimeDirectory = "C:/runtime";
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.75F;
    job.compressionRestorationTiled = tiled;
    Json manifest{{"schemaVersion",2},{"job",Json::parse(serializeJob(job))},{"identity",Json::object()},
        {"compressionRestorationRuntime",{
            {"modelId","stdf-r3-y7-sdr8"},{"adapterId","stdf-r3-y7-luma-v1"},{"adapterVersion",1},
            {"backend","onnxruntime_cuda"},{"signature","stdf-r3-y7-luma-v1;graph-test;ort-1.22.0"},
            {"descriptorSha256",job.expectedCompressionRestorationDescriptorSha256},{"strength",job.compressionRestorationStrength},
            {"inputProfile","bt709-sdr8"},{"pixelFormat","yuv420p"},{"maximumWidth",384},{"maximumHeight",256},
            {"tilePolicy",tiled ? restoration::stdf_tiles::policyId : "full-frame-bounded-384x256-v1"},
            {"decoderReplayPolicy","software-source-pts-from-start-v1"},
            {"sceneReplayPolicy","scene-bounds-serial-v1"}}}};
    if (tiled) {
        auto& runtime = manifest["compressionRestorationRuntime"];
        runtime["graphSha256"] = std::string(64,'a');
        runtime["signature"] = "stdf-r3-y7-luma-v1;graph-" + std::string(64,'a') +
            ";ort-1.22.0;tilePolicy-" + restoration::stdf_tiles::policyId;
        runtime["tiledPolicy"] = Json::parse(serializeCompressionRestorationPolicy(true));
    }
    return manifest;
}
}

TEST(CheckpointArtifacts, ContractRoundTripsExactRationalsAndColorExpectations) {
    rifelove::media::VideoSegmentContract contract;
    contract.frameCount = 121;
    contract.firstTimestamp = rifelove::Rational(1001,24000);
    contract.frameDuration = rifelove::Rational(1001,60000);
    contract.endExclusive = contract.firstTimestamp + contract.frameDuration * rifelove::Rational(120) +
        contract.frameDuration / rifelove::Rational(2);
    contract.width = 1920;
    contract.height = 1080;
    contract.codec = "h264";
    contract.encoder = "h264_nvenc";
    contract.pixelFormat = "yuv420p10le";
    contract.bitDepth = 10;
    contract.primaries = "bt709";
    contract.transfer = "bt709";
    contract.matrix = "bt709";
    contract.range = "tv";
    const auto restored = rifelove::application::CheckpointArtifacts::parseContract(
        rifelove::application::CheckpointArtifacts::serializeContract(contract));
    EXPECT_EQ(restored.frameCount,contract.frameCount);
    EXPECT_EQ(restored.firstTimestamp,contract.firstTimestamp);
    EXPECT_EQ(restored.frameDuration,contract.frameDuration);
    EXPECT_EQ(restored.endExclusive,contract.endExclusive);
    EXPECT_EQ(restored.width,contract.width);
    EXPECT_EQ(restored.pixelFormat,contract.pixelFormat);
    EXPECT_EQ(restored.encoder,contract.encoder);
    EXPECT_EQ(restored.primaries,contract.primaries);
    EXPECT_EQ(restored.range,contract.range);
    EXPECT_THROW((void)rifelove::application::CheckpointArtifacts::parseContract("{\"schemaVersion\":2}"),std::invalid_argument);
}

TEST(MediaPipelineIdentity, HashesActualLoadedDllContentsAndWriterImplementation) {
    using namespace rifelove;
    using namespace rifelove::application;
    const auto identity = nlohmann::json::parse(CheckpointArtifacts::mediaPipelineIdentity());
    EXPECT_EQ(identity.at("schemaVersion"),2);
    EXPECT_EQ(identity.at("writerSourceSha256").get<std::string>().size(),64U);
    EXPECT_NE(identity.at("writerAlgorithm").get<std::string>().find("bicubic-layout"),std::string::npos);
    const auto& pipeline = identity.at("processingPipeline");
    EXPECT_EQ(pipeline.at("schemaVersion"),1);
    EXPECT_EQ(pipeline.at("algorithmVersion"),"rife-processing-source-contract-v1");
    EXPECT_GE(pipeline.at("sourceFiles").size(),70U);
    for (const auto* source : {"src/media/rife_color.cpp","src/media/rife_color.hpp","src/timeline/timeline.cpp",
             "src/timeline/timeline.hpp","src/interpolation/scene_analysis.cpp","src/interpolation/scene_analysis.hpp",
             "src/core/pipeline/video_pipeline.cpp","src/core/pipeline/video_pipeline.hpp","src/restoration/noise_estimate.cpp",
             "src/restoration/noise_estimate.hpp","src/restoration/temporal_window.cpp","src/restoration/temporal_window.hpp"})
        EXPECT_TRUE(pipeline.at("sourceFiles").contains(source)) << source;
    EXPECT_EQ(pipeline.at("sourceFiles").at("src/media/video_writer.cpp"),identity.at("writerSourceSha256"));
    EXPECT_EQ(pipeline.at("build").at("cxxStandard"),20);
    // Obtain independent DLL data addresses, rather than reproducing the
    // configuration-string lookup used by production code.
    const auto inspect = [&](const char* name, const void* data) {
        HMODULE module = nullptr;
        ASSERT_TRUE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(data),&module));
        ASSERT_NE(module,GetModuleHandleW(nullptr));
        std::array<wchar_t,32768> path{};
        const auto length = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        ASSERT_GT(length,0U); ASSERT_LT(length,path.size());
        const auto file = std::filesystem::path(std::wstring(path.data(),length));
        EXPECT_EQ(identity.at("libraries").at(name).at("binarySha256"),inference::sha256File(file,512 * 1024 * 1024));
        EXPECT_EQ(identity.at("libraries").at(name).at("binarySize"),std::filesystem::file_size(file));
        EXPECT_FALSE(identity.at("libraries").at(name).contains("path"));
    };
    inspect("avcodec",avcodec_get_class());
    inspect("avformat",avformat_get_class());
    inspect("avutil",av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));
    inspect("swscale",sws_get_class());
    for (const auto& [name,library] : identity.at("dependencies").items()) {
        const auto encoded = std::filesystem::path(std::u8string(name.begin(),name.end()));
        const auto module = GetModuleHandleW(encoded.c_str());
        ASSERT_NE(module,nullptr) << name;
        std::array<wchar_t,32768> path{};
        const auto length = GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        ASSERT_GT(length,0U); ASSERT_LT(length,path.size());
        const auto file = std::filesystem::path(std::wstring(path.data(),length));
        EXPECT_EQ(library.at("binarySha256"),inference::sha256File(file,512 * 1024 * 1024)) << name;
    }
    for (const auto* codec : {L"libopenh264-8.dll",L"libkvazaar.dll",L"libaom.dll",L"swresample-6.dll"}) {
        if (!GetModuleHandleW(codec)) continue;
        const auto name = std::filesystem::path(codec).string();
        EXPECT_TRUE(identity.at("dependencies").contains(name)) << "Loaded linked media dependency was omitted: " << name;
    }
    RecordProperty("loaded_media_identity",identity.dump());
    RecordProperty("ffmpeg_reported_version",media::MediaEngine::runtimeVersion());
    EXPECT_EQ(nlohmann::json::parse(CheckpointArtifacts::mediaPipelineIdentity()),identity);
    std::stop_source cancel; cancel.request_stop();
    EXPECT_THROW((void)CheckpointArtifacts::mediaPipelineIdentity(cancel.get_token()),std::runtime_error);
}

TEST(MediaPipelineIdentity, RejectsVersionOnlyEvidenceEveryChangedDllAndWriterAlgorithm) {
    using namespace rifelove::application;
    using Json = nlohmann::json;
    const Json current{{"schemaVersion",2},{"mediaRuntime",rifelove::media::MediaEngine::runtimeVersion()},
        {"mediaPipelineIdentity",Json::parse(CheckpointArtifacts::mediaPipelineIdentity())}};
    EXPECT_NO_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(current.dump(),current.dump()));
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(current.dump(),current.dump()));
    auto legacy = current; legacy.erase("mediaPipelineIdentity");
    EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(legacy.dump(),current.dump()),std::logic_error);
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(current.dump(),legacy.dump()));
    for (const auto* name : {"avcodec","avformat","avutil","swscale"}) {
        auto changed = current;
        auto digest = changed["mediaPipelineIdentity"]["libraries"][name]["binarySha256"].get<std::string>();
        digest[0] = digest[0] == '0' ? '1' : '0';
        changed["mediaPipelineIdentity"]["libraries"][name]["binarySha256"] = digest;
        // Both human-readable/version numbers remain the same.
        EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changed.dump(),current.dump()),std::logic_error) << name;
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),current.dump())) << name;
        changed = current;
        changed["mediaPipelineIdentity"]["libraries"][name].erase("binarySha256");
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump())) << name;
    }
    for (const auto* field : {"writerAlgorithm","writerSourceSha256"}) {
        auto changed = current;
        changed["mediaPipelineIdentity"][field] = field == std::string("writerSourceSha256") ? std::string(64,'0') : "previous-writer";
        EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changed.dump(),current.dump()),std::logic_error);
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),current.dump()));
    }
    auto changedDependency = current;
    auto& dependency = changedDependency["mediaPipelineIdentity"]["dependencies"];
    if (dependency.empty()) dependency["added-codec.dll"] = {{"binarySha256",std::string(64,'0')},{"binarySize",std::uint64_t{1}}};
    else dependency.begin().value()["binarySha256"] = std::string(64,'0');
    EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changedDependency.dump(),current.dump()),std::logic_error);
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changedDependency.dump(),current.dump()));
}

TEST(MediaPipelineIdentity, QueueResumesOnlyMatchingProofAndPreservesRejectedSegmentsAndRecords) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    for (int change = 0; change < 8; ++change) {
        SCOPED_TRACE(change);
        CheckpointFixture fixture;
        const auto database = fixture.directory / "queue.sqlite", output = fixture.directory / "result.mkv";
        ProcessingJob job;
        job.source = fixture.source.string(); job.output = output.string();
        job.modelDirectory = fixture.directory / "model"; job.runtimeDirectory = fixture.directory / "runtime";
        job.settings.targetRate = Rational(50);
        ProcessingIdentity identity{};
        { SourceLease lease(fixture.source); identity.source = lease.fingerprint({}); }
        identity.modelId = "rife-fixture"; identity.modelVersion = "1"; identity.modelSha256 = std::string(64,'a');
        identity.backend = "onnxruntime_cuda"; identity.device.name = "CPU-only resume fixture";
        const auto identityText = serializeIdentity(identity);
        applyStoredIdentity(job,identityText);
        const auto configuration = serializeJob(job);
        Json manifest{{"schemaVersion",2},{"source",job.source},{"output",job.output},{"job",Json::parse(configuration)},
            {"identity",Json::parse(identityText)},{"mediaRuntime",media::MediaEngine::runtimeVersion()},
            {"mediaPipelineIdentity",Json::parse(CheckpointArtifacts::mediaPipelineIdentity())},{"framesPerSegment",256}};
        if (change == 1) manifest.erase("mediaPipelineIdentity");
        if (change == 2) manifest["mediaPipelineIdentity"]["libraries"]["swscale"]["binarySha256"] = std::string(64,'0');
        if (change == 3) manifest["mediaPipelineIdentity"]["writerAlgorithm"] = "old-color-scaler";
        if (change == 4 || change == 5) {
            auto& pipeline = manifest["mediaPipelineIdentity"]["processingPipeline"];
            pipeline["sourceFiles"][change == 4 ? "src/media/rife_color.cpp" : "src/timeline/timeline.hpp"] = std::string(64,'0');
            resealProcessingProof(pipeline);
        }
        if (change == 6) {
            manifest["mediaPipelineIdentity"]["schemaVersion"] = 1;
            manifest["mediaPipelineIdentity"].erase("processingPipeline");
        }
        if (change == 7) {
            auto& pipeline = manifest["mediaPipelineIdentity"]["processingPipeline"];
            pipeline["algorithmVersion"] = "previous-processing-algorithm"; resealProcessingProof(pipeline);
        }
        std::string taskId, segmentHash;
        std::filesystem::path segmentPath, uncommitted;
        {
            TaskStore seed(database.string());
            taskId = seed.createConfigured({{job.source,job.output,configuration}}).front();
            seed.transition(taskId,TaskState::running); seed.saveIdentity(taskId,identityText);
            CheckpointArtifacts artifacts(seed,taskId,output); artifacts.setManifest(manifest.dump());
            const auto segment = createSegment(fixture,artifacts);
            segmentPath = std::filesystem::path(segment.path);
            segmentHash = artifacts.commit(0,0,2,segment).sha256;
            if (change != 0) { uncommitted = artifacts.segmentPath(99); std::ofstream marker(uncommitted); marker << "must remain untouched"; }
            EXPECT_EQ(seed.recoverConfigured(),1U);
        }
        TaskQueue queue(database);
        if (change == 0) EXPECT_NO_THROW(queue.resumeCheckpoint(taskId));
        else {
            try { queue.resumeCheckpoint(taskId); FAIL() << "Incompatible media proof was reused"; }
            catch (const std::logic_error& error) { EXPECT_NE(std::string(error.what()).find("new output"),std::string::npos); }
            EXPECT_TRUE(std::filesystem::exists(uncommitted));
        }
        const auto rows = queue.list(); ASSERT_EQ(rows.size(),1U);
        EXPECT_EQ(rows.front().task.state,change == 0 ? TaskState::queued : TaskState::interrupted);
        EXPECT_EQ(rows.front().configuration,configuration); EXPECT_EQ(rows.front().identity,identityText);
        EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),manifest.dump());
        EXPECT_EQ(inference::sha256File(segmentPath,16 * 1024 * 1024),segmentHash);
        EXPECT_TRUE(std::filesystem::exists(database)); EXPECT_FALSE(std::filesystem::exists(output));
    }
}

TEST(CheckpointArtifacts, QueueResumesSchemaThirteenAutomaticDetailWithCommittedSegment) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    CheckpointFixture fixture;
    const auto database = fixture.directory / "legacy-detail-queue.sqlite";
    const auto output = fixture.directory / "legacy-detail-output.mkv";
    ProcessingJob job;
    job.source = fixture.source.string(); job.output = output.string();
    job.modelDirectory = fixture.directory / "missing-rife-model";
    job.runtimeDirectory = fixture.directory / "missing-rife-runtime";
    job.restorationModelDirectory = fixture.directory / "missing-deblur-model";
    job.expectedRestorationDescriptorSha256 = std::string(64,'d');
    job.settings.targetRate = Rational(50);
    job.settings.automaticDetailRestoration.emplace();
    ProcessingIdentity identity{};
    { SourceLease lease(fixture.source); identity.source = lease.fingerprint({}); }
    identity.modelId = "rife-fixture"; identity.modelVersion = "1";
    identity.modelSha256 = std::string(64,'a');
    identity.backend = "onnxruntime_cuda"; identity.device.name = "CPU-only detail resume fixture";
    const auto identityText = serializeIdentity(identity);
    applyStoredIdentity(job,identityText);
    auto legacyJob = Json::parse(serializeJob(job));
    legacyJob["schemaVersion"] = 13;
    legacyJob.erase("deblurModelDirectory");
    legacyJob.erase("expectedDeblurDescriptorSha256");
    legacyJob.erase("outputImageOrder");
    const auto configuration = legacyJob.dump();
    Json manifest{{"schemaVersion",2},{"source",job.source},{"output",job.output},{"job",legacyJob},
        {"identity",Json::parse(identityText)},{"mediaRuntime",media::MediaEngine::runtimeVersion()},
        {"mediaPipelineIdentity",Json::parse(CheckpointArtifacts::mediaPipelineIdentity())},
        {"framesPerSegment",256},
        {"restorationRuntime",{{"modelId","nafnet-fixture"},{"adapterId","nafnet-deblur-v1"},
            {"adapterVersion",1},{"backend","onnxruntime_cuda"},{"signature","graph-fixture;ort-1.22.0"},
            {"scaleNumerator",1},{"scaleDenominator",1}}}};
    manifest["restorationRuntime"]["automaticControl"] = legacyJob.at("automaticDetailRestoration");
    std::string taskId, segmentHash;
    std::filesystem::path segmentPath;
    {
        TaskStore seed(database.string());
        taskId = seed.createConfigured({{job.source,job.output,configuration}}).front();
        seed.transition(taskId,TaskState::running);
        seed.saveIdentity(taskId,identityText);
        CheckpointArtifacts artifacts(seed,taskId,output);
        artifacts.setManifest(manifest.dump());
        const auto segment = createSegment(fixture,artifacts);
        segmentPath = segment.path;
        segmentHash = artifacts.commit(0,0,2,segment).sha256;
        ASSERT_EQ(seed.recoverConfigured(),1U);
    }
    TaskQueue queue(database);
    EXPECT_NO_THROW(queue.resumeCheckpoint(taskId));
    const auto rows = queue.list();
    ASSERT_EQ(rows.size(),1U);
    EXPECT_EQ(rows.front().task.state,TaskState::queued);
    EXPECT_EQ(rows.front().configuration,configuration);
    EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),manifest.dump());
    EXPECT_EQ(inference::sha256File(segmentPath,16 * 1024 * 1024),segmentHash);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(MediaPipelineIdentity, ProcessingSourceHeaderAlgorithmAndBuildChangesFailClosed) {
    using namespace rifelove::application;
    using Json = nlohmann::json;
    const Json current{{"schemaVersion",2},{"mediaPipelineIdentity",Json::parse(CheckpointArtifacts::mediaPipelineIdentity())}};
    for (const auto* source : {"src/media/rife_color.cpp","src/media/rife_color.hpp","src/timeline/timeline.hpp",
             "src/core/pipeline/video_pipeline.cpp","src/interpolation/scene_analysis.cpp","src/restoration/temporal_window.cpp"}) {
        auto changed = current;
        auto& pipeline = changed["mediaPipelineIdentity"]["processingPipeline"];
        pipeline["sourceFiles"][source] = std::string(64,'0'); resealProcessingProof(pipeline);
        EXPECT_EQ(changed["mediaPipelineIdentity"]["writerSourceSha256"],current["mediaPipelineIdentity"]["writerSourceSha256"]);
        EXPECT_NO_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changed.dump(),changed.dump()));
        EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changed.dump(),current.dump()),std::logic_error) << source;
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),current.dump()));
    }
    for (const auto* field : {"compilerVersion","buildType","cxxFlagsSha256"}) {
        auto changed = current;
        changed["mediaPipelineIdentity"]["processingPipeline"]["build"][field] =
            field == std::string("cxxFlagsSha256") ? std::string(64,'0') : "other";
        EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(changed.dump(),current.dump()),std::logic_error);
    }
    auto corrupt = current;
    corrupt["mediaPipelineIdentity"]["processingPipeline"]["sourceSha256"] = std::string(64,'0');
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(corrupt.dump(),corrupt.dump()));
    auto previous = current;
    previous["mediaPipelineIdentity"]["schemaVersion"] = 1;
    previous["mediaPipelineIdentity"].erase("processingPipeline");
    EXPECT_THROW(CheckpointArtifacts::requireMediaPipelineIdentity(previous.dump(),current.dump()),std::logic_error);
}

TEST(MediaPipelineIdentity, LegacyPublishedOutputsReconcileWithoutReusingUnboundSegments) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    // An already published complete file is distinct from stitching unfinished
    // historical segments into a new encode. Only its saved validation/hash
    // proof can authorize adoption; a version-only manifest cannot do so.
    for (int history = 0; history < 4; ++history) {
        SCOPED_TRACE(history);
        CheckpointFixture fixture;
        const auto database = fixture.directory / "queue.sqlite", output = fixture.directory / "published.mkv";
        ProcessingJob job;
        job.source = fixture.source.string(); job.output = output.string();
        job.modelDirectory = fixture.directory / "missing-historical-model";
        job.runtimeDirectory = fixture.directory / "missing-historical-runtime";
        const auto configuration = serializeJob(job);
        const std::string historicalIdentity = "{\"schemaVersion\":1,\"historical\":true}";
        const Json manifest{{"schemaVersion",2},{"mediaRuntime","8.1.3"},{"historical",true}};
        std::string taskId, segmentHash, publishedHash, result;
        std::filesystem::path segmentPath;
        {
            TaskStore seed(database.string());
            taskId = seed.createConfigured({{job.source,job.output,configuration}}).front();
            seed.transition(taskId,TaskState::running); seed.saveIdentity(taskId,historicalIdentity);
            CheckpointArtifacts artifacts(seed,taskId,output); artifacts.setManifest(manifest.dump());
            const auto segment = createSegment(fixture,artifacts);
            segmentPath = std::filesystem::path(segment.path);
            segmentHash = artifacts.commit(0,0,2,segment).sha256;
            std::filesystem::copy_file(segmentPath,output);
            const auto validation = media::MediaEngine::verifyVideoSegment(output.string(),segment.contract);
            result = Json{{"schemaVersion",1},{"outputFrames",2},{"aiFrames",0},{"originalFrames",2},{"heldFrames",0},
                {"validation",{{"decodedFrames",validation.frameCount}}}}.dump();
            publishedHash = inference::sha256File(output,16 * 1024 * 1024);
            if (history != 3)
                seed.saveOutputPublicationIntent(taskId,publishedHash,std::filesystem::file_size(output),result);
            if (history == 1) seed.finish(taskId,TaskState::completed,result);
            if (history == 2) { std::ofstream changed(output,std::ios::binary | std::ios::app); changed << "changed after publication"; }
        }
        const auto actualPublishedHash = inference::sha256File(output,16 * 1024 * 1024);
        {
            TaskQueue queue(database);
            const auto rows = queue.list(); ASSERT_EQ(rows.size(),1U);
            EXPECT_EQ(queue.recoveredPublishedOutputCount(),history == 0 ? 1U : 0U);
            EXPECT_EQ(rows.front().task.state,history < 2 ? TaskState::completed : TaskState::interrupted);
            EXPECT_EQ(rows.front().result,history < 2 ? result : "");
            if (history == 2) EXPECT_NE(rows.front().task.error.find("does not match the saved validation proof"),std::string::npos);
            EXPECT_EQ(rows.front().configuration,configuration);
            EXPECT_EQ(rows.front().identity,historicalIdentity);
            EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),manifest.dump());
            EXPECT_EQ(inference::sha256File(segmentPath,16 * 1024 * 1024),segmentHash);
            EXPECT_EQ(inference::sha256File(output,16 * 1024 * 1024),actualPublishedHash);
            EXPECT_TRUE(std::filesystem::exists(fixture.source));
        }
        // Reopening a second time must not re-adopt or reinterpret history.
        TaskQueue reopened(database);
        EXPECT_EQ(reopened.recoveredPublishedOutputCount(),0U);
        EXPECT_EQ(reopened.list().front().task.state,history < 2 ? TaskState::completed : TaskState::interrupted);
    }
}

TEST(CheckpointArtifacts, AutomaticNoiseIdentityRejectsEveryChangedParameterAndFixedSigmaHistory) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    ProcessingJob job;
    job.source = "C:/source.mkv"; job.output = "C:/output.mkv";
    job.modelDirectory = "C:/model"; job.runtimeDirectory = "C:/runtime";
    Json fixed{{"schemaVersion",2},{"job",Json::parse(serializeJob(job))},{"identity",Json::object()}};
    auto versionEight = fixed;
    versionEight["job"]["schemaVersion"] = 8;
    versionEight["job"].erase("automaticTemporalDenoise");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(versionEight.dump(),fixed.dump()));
    job.settings.automaticTemporalDenoise.emplace();
    auto automatic = fixed; automatic["job"] = Json::parse(serializeJob(job));
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(automatic.dump(),automatic.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fixed.dump(),automatic.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(versionEight.dump(),automatic.dump()));
    for (const auto* field : {"relativeAdjustment","smoothingSeconds","maximumSigmaChangePerSecond","minimumConfidence"}) {
        auto changed = automatic;
        changed["job"]["automaticTemporalDenoise"][field] = 0.12345;
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump())) << field;
    }
    auto changed = automatic; changed["job"]["automaticTemporalDenoise"]["algorithmVersion"] = 1;
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),automatic.dump()));
    changed = automatic; changed["job"]["automaticTemporalDenoise"]["replayPolicy"] = "other-decoder";
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump()));
    changed = automatic; changed["identity"]["source"] = "changed source";
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump()));
    changed = automatic; changed["temporalRestorationRuntime"] = {{"signature","changed runtime"}};
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump()));
}

TEST(CheckpointArtifacts, AutomaticDetailPinsModelAnalyzerSettingsAndSourceReplay) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    ProcessingJob job;
    job.source = "C:/source.mkv"; job.output = "C:/output.mkv";
    job.modelDirectory = "C:/rife"; job.runtimeDirectory = "C:/runtime";
    job.restorationModelDirectory = "C:/deblur";
    job.expectedRestorationDescriptorSha256 = std::string(64,'a');
    job.settings.automaticDetailRestoration.emplace();
    Json enabled{{"schemaVersion",2},{"job",Json::parse(serializeJob(job))},{"identity",Json::object()},
        {"restorationRuntime",{{"modelId","test-detail"},{"adapterId","test-deblur-v1"},
            {"adapterVersion",1},{"backend","onnxruntime_cuda"},{"signature","test-runtime-v1"}}}};
    enabled["restorationRuntime"]["automaticControl"] = enabled.at("job").at("automaticDetailRestoration");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(enabled.dump(),enabled.dump()));
    EXPECT_TRUE(parseJob(job.source,job.output,enabled.at("job").dump()).settings.automaticDetailRestoration.has_value());
    for (const auto* field : {"relativeAdjustment","smoothingSeconds","maximumStrengthChangePerSecond","minimumConfidence"}) {
        auto changed = enabled;
        changed["job"]["automaticDetailRestoration"][field] = 0.12345;
        changed["restorationRuntime"]["automaticControl"] = changed.at("job").at("automaticDetailRestoration");
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),changed.dump())) << field;
    }
    auto changed = enabled; changed["restorationRuntime"].erase("automaticControl");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    changed = enabled; changed["job"]["automaticDetailRestoration"]["replayPolicy"] = "nvdec";
    EXPECT_THROW((void)parseJob(job.source,job.output,changed.at("job").dump()),std::invalid_argument);
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    changed = enabled; changed["job"]["expectedRestorationDescriptorSha256"] = std::string(64,'b');
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),changed.dump()));
    job.settings.automaticDetailRestoration.reset();
    auto disabled = enabled; disabled["job"] = Json::parse(serializeJob(job));
    disabled["restorationRuntime"].erase("automaticControl");
    auto prior = disabled; prior["job"]["schemaVersion"] = 12; prior["job"].erase("automaticDetailRestoration");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(prior.dump(),disabled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(prior.dump(),enabled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),disabled.dump()));
}
TEST(CheckpointArtifacts, IndependentDeblurBindsDescriptorRuntimeOrderAndLegacyMigration) {
    using namespace rifelove::application;
    using Json = nlohmann::json;
    ProcessingJob job;
    job.source = "C:/source.mkv"; job.output = "C:/output.mkv";
    job.modelDirectory = "C:/rife"; job.runtimeDirectory = "C:/runtime";
    job.restorationModelDirectory = "C:/sr";
    job.expectedRestorationDescriptorSha256 = std::string(64,'a');
    job.deblurModelDirectory = "C:/deblur";
    job.expectedDeblurDescriptorSha256 = std::string(64,'b');
    job.settings.automaticDetailRestoration.emplace();
    Json dual{{"schemaVersion",2},{"job",Json::parse(serializeJob(job))},{"identity",Json::object()},
        {"outputImageOrder","deblur-before-super-resolution-v1"},
        {"restorationRuntime",{{"modelId","test-sr"},{"adapterId","test-sr-v1"},
            {"adapterVersion",1},{"backend","onnxruntime_cuda"},{"signature","sr-runtime"},
            {"scaleNumerator",2},{"scaleDenominator",1}}},
        {"deblurRuntime",{{"modelId","test-deblur"},{"adapterId","test-deblur-v1"},
            {"adapterVersion",1},{"backend","onnxruntime_cuda"},{"signature","deblur-runtime"},
            {"scaleNumerator",1},{"scaleDenominator",1},{"task","deblur"},
            {"descriptorSha256",std::string(64,'b')}}}};
    dual["deblurRuntime"]["automaticControl"] = dual.at("job").at("automaticDetailRestoration");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(dual.dump(),dual.dump()));
    auto changed = dual; changed["deblurRuntime"]["signature"] = "different";
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(dual.dump(),changed.dump()));
    changed = dual; changed["deblurRuntime"]["descriptorSha256"] = std::string(64,'c');
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    changed = dual; changed["outputImageOrder"] = "super-resolution-before-deblur-v1";
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    changed = dual; changed.erase("deblurRuntime");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    changed = dual; changed["restorationRuntime"]["automaticControl"] = changed["deblurRuntime"].at("automaticControl");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));

    auto disabledJob = job;
    disabledJob.deblurModelDirectory.clear();
    disabledJob.expectedDeblurDescriptorSha256.clear();
    disabledJob.settings.automaticDetailRestoration.reset();
    auto current = dual;
    current["job"] = Json::parse(serializeJob(disabledJob));
    current.erase("deblurRuntime"); current.erase("outputImageOrder");
    auto schemaThirteen = current;
    schemaThirteen["job"]["schemaVersion"] = 13;
    schemaThirteen["job"].erase("deblurModelDirectory");
    schemaThirteen["job"].erase("expectedDeblurDescriptorSha256");
    schemaThirteen["job"].erase("outputImageOrder");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(schemaThirteen.dump(),current.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(schemaThirteen.dump(),dual.dump()));
}

TEST(CheckpointArtifacts, CompressionRestorationIdentityRejectsChangedModelStrengthRuntimeShapeAndReplayPolicy) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    ProcessingJob job;
    job.source = "C:/source.mkv"; job.output = "C:/output.mkv";
    job.modelDirectory = "C:/model"; job.runtimeDirectory = "C:/runtime";
    job.compressionRestorationModelDirectory = "C:/models/stdf-r3";
    job.expectedCompressionRestorationDescriptorSha256 = std::string(64,'d');
    job.compressionRestorationStrength = 0.75F;
    Json enabled{{"schemaVersion",2},{"job",Json::parse(serializeJob(job))},{"identity",Json::object()},
        {"compressionRestorationRuntime",{
            {"modelId","stdf-r3-y7-sdr8"},{"adapterId","stdf-r3-y7-luma-v1"},{"adapterVersion",1},
            {"backend","onnxruntime_cuda"},{"signature","stdf-r3-y7-luma-v1;graph-test;ort-1.22.0"},
            {"descriptorSha256",std::string(64,'d')},{"strength",0.75F},
            {"inputProfile","bt709-sdr8"},{"pixelFormat","yuv420p"},{"maximumWidth",384},{"maximumHeight",256},
            {"tilePolicy","full-frame-bounded-384x256-v1"},
            {"decoderReplayPolicy","software-source-pts-from-start-v1"},
            {"sceneReplayPolicy","scene-bounds-serial-v1"}}}};
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(enabled.dump(),enabled.dump()));
    for (const auto* field : {"descriptorSha256","strength","inputProfile","pixelFormat","maximumWidth","maximumHeight",
                              "tilePolicy","decoderReplayPolicy","sceneReplayPolicy","signature"}) {
        auto changed = enabled;
        if (std::string(field) == "strength") changed["compressionRestorationRuntime"][field] = 0.5F;
        else if (std::string(field) == "maximumWidth") changed["compressionRestorationRuntime"][field] = 512;
        else changed["compressionRestorationRuntime"][field] = "changed";
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),changed.dump())) << field;
    }
    auto changedJob = enabled;
    changedJob["job"]["compressionRestorationStrength"] = 0.5F;
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),changedJob.dump()));
    auto missingPolicy = enabled;
    missingPolicy["compressionRestorationRuntime"].erase("decoderReplayPolicy");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(enabled.dump(),missingPolicy.dump()));

    auto disabled = enabled;
    disabled["job"]["schemaVersion"] = 10;
    disabled["job"]["compressionRestorationModelDirectory"] = "";
    disabled["job"]["expectedCompressionRestorationDescriptorSha256"] = "";
    disabled["job"]["compressionRestorationStrength"] = 0.0F;
    disabled.erase("compressionRestorationRuntime");
    auto legacy = disabled;
    legacy["job"]["schemaVersion"] = 9;
    legacy["job"].erase("compressionRestorationModelDirectory");
    legacy["job"].erase("expectedCompressionRestorationDescriptorSha256");
    legacy["job"].erase("compressionRestorationStrength");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(legacy.dump(),disabled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(legacy.dump(),enabled.dump()));
}

TEST(CheckpointArtifacts, AutomaticCompressionPinsSourceReplayAndCannotAdoptFixedStrengthHistory) {
    using namespace rifelove::application;
    using Json = nlohmann::json;
    const auto fixed = compressionManifest(false);
    auto legacy = fixed;
    legacy["job"]["schemaVersion"] = 11;
    legacy["job"].erase("automaticCompressionRestoration");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(legacy.dump(),fixed.dump()));
    auto automatic = fixed;
    automatic["job"]["automaticCompressionRestoration"] = {
        {"algorithmVersion",2},{"analyzer","sdr8-luma-block8-stratified-v2"},{"replayPolicy","software-source-pts-from-start-v1"},
        {"relativeAdjustment",-0.25},{"smoothingSeconds",0.35},{"maximumStrengthChangePerSecond",0.5},{"minimumConfidence",0.6}};
    automatic["compressionRestorationRuntime"]["automaticControl"] = automatic.at("job").at("automaticCompressionRestoration");
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(automatic.dump(),automatic.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fixed.dump(),automatic.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(legacy.dump(),automatic.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),fixed.dump()));
    for (const auto* field : {"relativeAdjustment","smoothingSeconds","maximumStrengthChangePerSecond","minimumConfidence"}) {
        auto changed = automatic;
        changed["job"]["automaticCompressionRestoration"][field] = 0.12345;
        changed["compressionRestorationRuntime"]["automaticControl"] = changed.at("job").at("automaticCompressionRestoration");
        EXPECT_TRUE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump())) << field;
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump())) << field;
    }
    const auto rejected = [&](const Json& changed) {
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(automatic.dump(),changed.dump()));
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    };
    for (const auto* field : {"algorithmVersion","analyzer","replayPolicy","relativeAdjustment","smoothingSeconds","maximumStrengthChangePerSecond","minimumConfidence"}) {
        auto changed = automatic; changed["job"]["automaticCompressionRestoration"].erase(field); rejected(changed);
    }
    auto changed = automatic; changed["compressionRestorationRuntime"].erase("automaticControl"); rejected(changed);
    changed = automatic; changed["compressionRestorationRuntime"]["automaticControl"]["relativeAdjustment"] = 0.1; rejected(changed);
    changed = automatic; changed["job"]["automaticCompressionRestoration"]["analyzer"] = "unvalidated"; rejected(changed);
    changed = automatic;
    changed["job"]["automaticCompressionRestoration"]["algorithmVersion"] = 1;
    changed["job"]["automaticCompressionRestoration"]["analyzer"] = "sdr8-luma-block8-excess-v1";
    changed["compressionRestorationRuntime"]["automaticControl"] = changed.at("job").at("automaticCompressionRestoration");
    rejected(changed);
    changed = automatic; changed["job"]["automaticCompressionRestoration"]["replayPolicy"] = "seek"; rejected(changed);
    changed = automatic; changed["job"]["schemaVersion"] = 11; rejected(changed);
    changed = automatic; changed.erase("compressionRestorationRuntime"); rejected(changed);
    changed = fixed; changed["compressionRestorationRuntime"]["automaticControl"] = Json::object(); rejected(changed);
}

TEST(CheckpointArtifacts, SchemaElevenPreservesLegacyFullFrameRuntimeButNeverMigratesItToTiling) {
    using namespace rifelove::application;
    const auto current = compressionManifest(false);
    auto legacy = current;
    legacy["job"]["schemaVersion"] = 10;
    legacy["job"].erase("compressionRestorationTiled");
    legacy["job"].erase("compressionRestorationPolicy");
    const auto savedRuntime = legacy.at("compressionRestorationRuntime").dump();
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));
    EXPECT_EQ(current.at("compressionRestorationRuntime").dump(),savedRuntime);
    EXPECT_EQ(current.at("compressionRestorationRuntime").at("tilePolicy"),"full-frame-bounded-384x256-v1");
    EXPECT_FALSE(current.at("compressionRestorationRuntime").contains("graphSha256"));
    EXPECT_FALSE(current.at("compressionRestorationRuntime").contains("tiledPolicy"));

    const auto tiled = compressionManifest(true);
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(tiled.dump(),tiled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(legacy.dump(),tiled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(current.dump(),tiled.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(tiled.dump(),current.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(tiled.dump(),legacy.dump()));

    auto disguised = tiled;
    disguised["job"]["schemaVersion"] = 10;
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(disguised.dump(),disguised.dump()));
    disguised["job"].erase("compressionRestorationTiled");
    disguised["job"].erase("compressionRestorationPolicy");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(disguised.dump(),disguised.dump()));
}

TEST(CheckpointArtifacts, TiledCompressionRejectsIncompleteAndChangedExecutionContractsEvenWhenSelfMatched) {
    using namespace rifelove::application;
    using Json = nlohmann::json;
    const auto original = compressionManifest(true);
    const auto rejected = [&](const Json& changed) {
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(original.dump(),changed.dump()));
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump()));
    };
    for (const auto* field : {"compressionRestorationTiled","compressionRestorationPolicy"}) {
        auto changed = original; changed["job"].erase(field); rejected(changed);
    }
    auto changed = original; changed["job"]["compressionRestorationTiled"] = 1; rejected(changed);
    changed = original; changed.erase("compressionRestorationRuntime"); rejected(changed);
    changed = original; changed["job"]["compressionRestorationModelDirectory"] = ""; rejected(changed);
    changed = original; changed["job"]["expectedCompressionRestorationDescriptorSha256"] = ""; rejected(changed);
    changed = original; changed["job"]["compressionRestorationStrength"] = 0.0; rejected(changed);
    for (const auto& [field,value] : original.at("job").at("compressionRestorationPolicy").items()) {
        SCOPED_TRACE(field);
        changed = original; changed["job"]["compressionRestorationPolicy"].erase(field); rejected(changed);
        changed = original; changed["job"]["compressionRestorationPolicy"][field] = "changed"; rejected(changed);
        changed = original; changed["compressionRestorationRuntime"]["tiledPolicy"].erase(field); rejected(changed);
        changed = original; changed["compressionRestorationRuntime"]["tiledPolicy"][field] = "changed"; rejected(changed);
    }
    changed = original; changed["job"]["compressionRestorationPolicy"]["extra"] = true; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["tiledPolicy"]["extra"] = true; rejected(changed);
    for (const auto* field : {"graphSha256","tiledPolicy","signature","backend","tilePolicy",
                              "descriptorSha256","strength","inputProfile","pixelFormat","maximumWidth","maximumHeight",
                              "decoderReplayPolicy","sceneReplayPolicy"}) {
        SCOPED_TRACE(field);
        changed = original; changed["compressionRestorationRuntime"].erase(field); rejected(changed);
    }
    for (const auto* field : {"tilePolicy","inputProfile","pixelFormat","decoderReplayPolicy","sceneReplayPolicy"}) {
        SCOPED_TRACE(field);
        changed = original; changed["compressionRestorationRuntime"][field] = "changed"; rejected(changed);
    }
    changed = original; changed["compressionRestorationRuntime"]["graphSha256"] = std::string(64,'b'); rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["signature"] = "legacy-full-frame-runtime"; rejected(changed);
    const auto signature = original.at("compressionRestorationRuntime").at("signature").get<std::string>();
    changed = original; changed["compressionRestorationRuntime"]["signature"] = signature + "-unknown-policy-suffix"; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["signature"] = signature + ";graph-" + std::string(64,'a'); rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["signature"] = signature + ";tilePolicy-" +
        rifelove::restoration::stdf_tiles::policyId; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["signature"] = "graph-" + std::string(65,'a') +
        ";tilePolicy-" + rifelove::restoration::stdf_tiles::policyId; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["backend"] = "CPUExecutionProvider"; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["descriptorSha256"] = std::string(64,'b'); rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["strength"] = 0.5; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["maximumWidth"] = 512; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["maximumHeight"] = 384; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["maximumWidth"] = 384.0; rejected(changed);
    changed = original; changed["compressionRestorationRuntime"]["maximumHeight"] = 256.0; rejected(changed);
}

TEST(CheckpointArtifacts, TiledCompressionBindsValidGraphRuntimeModelAndAdapterIdentities) {
    using namespace rifelove::application;
    const auto original = compressionManifest(true);
    for (const auto* field : {"modelId","adapterId","adapterVersion","signature"}) {
        auto changed = original;
        if (std::string(field) == "adapterVersion") changed["compressionRestorationRuntime"][field] = 2;
        else changed["compressionRestorationRuntime"][field] =
            changed["compressionRestorationRuntime"][field].get<std::string>() + ";other-runtime";
        // A different otherwise valid runtime is its own identity and cannot
        // inherit segments from this one. The actual loaded backend signature
        // is checked again by TaskQueue before it reuses any frames.
        EXPECT_TRUE(CheckpointArtifacts::manifestMatches(changed.dump(),changed.dump())) << field;
        EXPECT_FALSE(CheckpointArtifacts::manifestMatches(original.dump(),changed.dump())) << field;
    }
    auto changedGraph = original;
    const std::string graph(64,'b');
    changedGraph["compressionRestorationRuntime"]["graphSha256"] = graph;
    changedGraph["compressionRestorationRuntime"]["signature"] = "graph-" + graph + ";tilePolicy-" +
        rifelove::restoration::stdf_tiles::policyId;
    EXPECT_TRUE(CheckpointArtifacts::manifestMatches(changedGraph.dump(),changedGraph.dump()));
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(original.dump(),changedGraph.dump()));

    auto fullFrame = compressionManifest(false);
    fullFrame["compressionRestorationRuntime"]["tiledPolicy"] = original.at("compressionRestorationRuntime").at("tiledPolicy");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fullFrame.dump(),fullFrame.dump()));
    fullFrame = compressionManifest(false);
    fullFrame["compressionRestorationRuntime"]["graphSha256"] = graph;
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fullFrame.dump(),fullFrame.dump()));
    fullFrame = compressionManifest(false);
    fullFrame["compressionRestorationRuntime"]["signature"] = original.at("compressionRestorationRuntime").at("signature");
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fullFrame.dump(),fullFrame.dump()));
    fullFrame = compressionManifest(false);
    fullFrame["compressionRestorationRuntime"]["signature"] = "full-frame-runtime;tilePolicy-unrecognized-tiled-policy";
    EXPECT_FALSE(CheckpointArtifacts::manifestMatches(fullFrame.dump(),fullFrame.dump()));
}

TEST(CheckpointArtifacts, TaskQueueResumesEnabledCompressionCheckpointAfterLoadingSavedRuntimeIdentity) {
    using namespace rifelove;
    using namespace rifelove::application;
    using Json = nlohmann::json;
    const auto packPath = findLocalStdfPack();
    if (!packPath) GTEST_SKIP() << "local STDF evaluation model pack is not available";
    const auto model = restoration::loadModelPack(*packPath);
    ASSERT_TRUE(restoration::supportsStdfCudaModel(model));

    // Preflight needs the pack's immutable descriptor/graph, but no inference
    // session or private models are required on CI (this test skips there).
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario == 0 ? "schema11 full-frame" : scenario == 1 ? "schema10 full-frame" : "schema11 tiled");
        CheckpointFixture fixture;
        const auto database = fixture.directory / "queue.sqlite", output = fixture.directory / "result.mkv";
        ProcessingJob job;
        job.source = fixture.source.string(); job.output = output.string();
        job.modelDirectory = fixture.directory / "missing-rife-model";
        job.runtimeDirectory = fixture.directory / "missing-rife-runtime";
        job.compressionRestorationModelDirectory = *packPath;
        job.expectedCompressionRestorationDescriptorSha256 = inference::sha256File(*packPath / "model.json",256 * 1024);
        job.compressionRestorationStrength = 0.75F;
        job.compressionRestorationTiled = scenario == 2;
        job.settings.targetRate = Rational(50);
        ProcessingIdentity identity{};
        { SourceLease lease(fixture.source); identity.source = lease.fingerprint({}); }
        identity.modelId = "rife-fixture"; identity.modelVersion = "1"; identity.modelSha256 = std::string(64,'a');
        identity.backend = "onnxruntime_cuda"; identity.device.name = "CPU-only compression resume fixture";
        applyStoredIdentity(job,serializeIdentity(identity));
        auto configurationJson = Json::parse(serializeJob(job));
        if (scenario == 1) {
            configurationJson["schemaVersion"] = 10;
            configurationJson.erase("compressionRestorationTiled");
            configurationJson.erase("compressionRestorationPolicy");
        }
        const auto configuration = configurationJson.dump();
        const auto identityText = serializeIdentity(identity);
        Json manifest{{"schemaVersion",2},{"source",job.source},{"output",job.output},{"job",configurationJson},
            {"identity",Json::parse(identityText)},{"mediaRuntime",media::MediaEngine::runtimeVersion()},
            {"mediaPipelineIdentity",Json::parse(CheckpointArtifacts::mediaPipelineIdentity())},{"framesPerSegment",256},
            {"compressionRestorationRuntime",{
                {"modelId",model.id},{"adapterId",model.capabilities.adapterId},{"adapterVersion",model.capabilities.adapterVersion},
                {"backend","onnxruntime_cuda"},{"signature","stdf-r3-y7-luma-v1;resume-fixture-runtime"},
                {"descriptorSha256",job.expectedCompressionRestorationDescriptorSha256},{"strength",job.compressionRestorationStrength},
                {"inputProfile","bt709-sdr8"},{"pixelFormat","yuv420p"},{"maximumWidth",384},{"maximumHeight",256},
                {"tilePolicy",job.compressionRestorationTiled ? restoration::stdf_tiles::policyId : "full-frame-bounded-384x256-v1"},
                {"decoderReplayPolicy","software-source-pts-from-start-v1"},
                {"sceneReplayPolicy","scene-bounds-serial-v1"}}}};
        if (job.compressionRestorationTiled) {
            auto& runtime = manifest["compressionRestorationRuntime"];
            runtime["graphSha256"] = model.graphSha256;
            runtime["signature"] = "stdf-r3-y7-luma-v1;graph-" + model.graphSha256 +
                ";resume-fixture-runtime;tilePolicy-" + restoration::stdf_tiles::policyId;
            runtime["tiledPolicy"] = Json::parse(serializeCompressionRestorationPolicy(true));
        }
        std::string taskId;
        {
            TaskStore seed(database.string());
            taskId = seed.createConfigured({{job.source,job.output,configuration}}).front();
            seed.transition(taskId,TaskState::running); seed.saveIdentity(taskId,identityText);
            CheckpointArtifacts artifacts(seed,taskId,output); artifacts.setManifest(manifest.dump());
            const auto segment = createSegment(fixture,artifacts);
            (void)artifacts.commit(0,0,2,segment);
            ASSERT_EQ(seed.recoverConfigured(),1U);
        }

        TaskQueue queue(database);
        EXPECT_NO_THROW(queue.resumeCheckpoint(taskId));
        const auto rows = queue.list();
        ASSERT_EQ(rows.size(),1U);
        EXPECT_EQ(rows.front().task.state,TaskState::queued);
        EXPECT_EQ(rows.front().configuration,configuration);
        EXPECT_EQ(rows.front().identity,identityText);
        EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),manifest.dump());
    }
}

TEST(CheckpointArtifacts, LegacyManifestUpgradeBindsOnlyTheBuiltinAdapter) {
    using Json = nlohmann::json;
    Json legacy{
        {"schemaVersion",1},{"source","C:/source.mkv"},{"output","C:/output.mkv"},
        {"job",{{"schemaVersion",1},{"expectedModelId","rife-4.25"},{"expectedModelVersion","4.25"},
            {"expectedModelSha256",std::string(64,'a')}}},
        {"identity",{{"schemaVersion",1},{"source",{{"sha256",std::string(64,'b')}}},
            {"model",{{"id","rife-4.25"},{"version","4.25"},{"sha256",std::string(64,'a')},
                {"inferenceBackend","onnxruntime_cuda"}}}}},
        {"mediaRuntime","ffmpeg fixture"},{"framesPerSegment",256}};
    Json current = legacy;
    current["schemaVersion"] = 2;
    current["job"]["schemaVersion"] = 4;
    current["job"]["inferenceBackend"] = "auto";
    current["job"]["expectedModelAdapterId"] = "rife-sdr-v1";
    current["job"]["expectedModelAdapterVersion"] = 1;
    current["job"]["restorationModelDirectory"] = "";
    current["job"]["expectedRestorationDescriptorSha256"] = "";
    current["identity"]["schemaVersion"] = 2;
    current["identity"]["model"]["adapter"] = {{"id","rife-sdr-v1"},{"version",1}};
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));

    auto currentV5 = current;
    currentV5["job"]["schemaVersion"] = 5;
    currentV5["job"]["motionBlur"] = {{"enabled",false},{"shutterAngleDegrees",180},{"sampleCount",4}};
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(current.dump(),currentV5.dump()));
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(legacy.dump(),currentV5.dump()));
    auto changedMotionBlur = currentV5;
    changedMotionBlur["job"]["motionBlur"]["enabled"] = true;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(current.dump(),changedMotionBlur.dump()));
    changedMotionBlur = currentV5;
    changedMotionBlur["job"]["motionBlur"]["sampleCount"] = 8;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(current.dump(),changedMotionBlur.dump()));

    auto oldFourFrameBlur = currentV5;
    oldFourFrameBlur["job"]["motionBlur"]["enabled"] = true;
    auto fullPtsBlur = oldFourFrameBlur;
    fullPtsBlur["job"]["motionBlur"]["algorithmVersion"] = 2;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(oldFourFrameBlur.dump(),fullPtsBlur.dump()));
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(fullPtsBlur.dump(),fullPtsBlur.dump()));

    auto currentV6 = currentV5;
    currentV6["job"]["schemaVersion"] = 6;
    currentV6["job"]["encoding"] = {{"outputWidth",0},{"outputHeight",0}};
    auto savedV5 = currentV6;
    savedV5["job"]["schemaVersion"] = 5;
    savedV5["job"]["encoding"].erase("outputWidth");
    savedV5["job"]["encoding"].erase("outputHeight");
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(savedV5.dump(),currentV6.dump()));
    auto savedV4 = savedV5;
    savedV4["job"]["schemaVersion"] = 4;
    savedV4["job"].erase("motionBlur");
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(savedV4.dump(),currentV6.dump()));
    auto changedTarget = currentV6;
    changedTarget["job"]["encoding"]["outputWidth"] = 3840;
    changedTarget["job"]["encoding"]["outputHeight"] = 2160;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV6.dump(),changedTarget.dump()));

    auto currentV7 = currentV6;
    currentV7["job"]["schemaVersion"] = 7;
    currentV7["job"]["temporalDenoiseSigma255"] = 25.0F;
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV6.dump(),currentV7.dump()));
    auto changedSigma = currentV7;
    changedSigma["job"]["temporalDenoiseSigma255"] = 17.5F;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV6.dump(),changedSigma.dump()));
    auto savedV5ForV7 = currentV7;
    savedV5ForV7["job"]["schemaVersion"] = 5;
    savedV5ForV7["job"].erase("temporalDenoiseSigma255");
    savedV5ForV7["job"]["encoding"].erase("outputWidth");
    savedV5ForV7["job"]["encoding"].erase("outputHeight");
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(savedV5ForV7.dump(),currentV7.dump()));
    auto savedV4ForV7 = savedV5ForV7;
    savedV4ForV7["job"]["schemaVersion"] = 4;
    savedV4ForV7["job"].erase("motionBlur");
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(savedV4ForV7.dump(),currentV7.dump()));

    auto currentV8 = currentV7;
    currentV8["job"]["schemaVersion"] = 8;
    currentV8["job"]["temporalRestorationModelDirectory"] = "";
    currentV8["job"]["expectedTemporalRestorationDescriptorSha256"] = "";
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV7.dump(),currentV8.dump()));
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(savedV4ForV7.dump(),currentV8.dump()));
    auto changedTemporalModel = currentV8;
    changedTemporalModel["job"]["temporalRestorationModelDirectory"] = "C:/model/denoise";
    changedTemporalModel["job"]["expectedTemporalRestorationDescriptorSha256"] = std::string(64,'a');
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV7.dump(),changedTemporalModel.dump()));
    auto changedRuntime = currentV8;
    changedRuntime["temporalRestorationRuntime"] = {{"modelId","denoise"}};
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(currentV7.dump(),changedRuntime.dump()));

    Json versionTwo = current;
    versionTwo["schemaVersion"] = 2;
    versionTwo["job"]["schemaVersion"] = 2;
    versionTwo["job"].erase("inferenceBackend");
    versionTwo["job"].erase("restorationModelDirectory");
    versionTwo["job"].erase("expectedRestorationDescriptorSha256");
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(versionTwo.dump(),current.dump()));
    auto changedBackend = current;
    changedBackend["job"]["inferenceBackend"] = "cuda";
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(versionTwo.dump(),changedBackend.dump()));

    current["job"]["expectedModelAdapterId"] = "custom-adapter";
    current["job"]["expectedModelAdapterVersion"] = 2;
    current["identity"]["model"]["adapter"] = {{"id","custom-adapter"},{"version",2}};
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));
    current["job"]["expectedModelAdapterId"] = "rife-sdr-v1";
    current["job"]["expectedModelAdapterVersion"] = 1;
    current["identity"]["model"]["adapter"] = {{"id","rife-sdr-v1"},{"version",1}};
    current["framesPerSegment"] = 128;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));
    auto legacyWithRestoration = legacy;
    legacyWithRestoration["job"]["restorationModelDirectory"] = "C:/local-model";
    legacyWithRestoration["job"]["expectedRestorationDescriptorSha256"] = std::string(64,'c');
    legacyWithRestoration["framesPerSegment"] = 128;
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(legacyWithRestoration.dump(),current.dump()));

    auto restoration = current;
    restoration["job"]["restorationModelDirectory"] = "C:/local-model";
    restoration["job"]["expectedRestorationDescriptorSha256"] = std::string(64,'c');
    restoration["restorationRuntime"] = {{"modelId","nafnet-local"},{"adapterId","nafnet-zoo-deblur-rgb-f32-nchw-v1"},
        {"adapterVersion",1},{"backend","onnxruntime_cuda"},{"signature","ort-1.22.0;tile-384"},
        {"scaleNumerator",1},{"scaleDenominator",1}};
    auto changedRestoration = restoration;
    changedRestoration["restorationRuntime"]["signature"] = "ort-1.22.0;tile-512";
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(restoration.dump(),restoration.dump()));
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(restoration.dump(),changedRestoration.dump()));
    changedRestoration = restoration;
    changedRestoration["job"]["expectedRestorationDescriptorSha256"] = std::string(64,'d');
    EXPECT_FALSE(rifelove::application::CheckpointArtifacts::manifestMatches(restoration.dump(),changedRestoration.dump()));
}

TEST(CheckpointArtifacts, TaskQueueRejectsUnboundLegacyMediaIdentityWithoutChangingArtifacts) {
    using namespace rifelove;
    using namespace rifelove::application;
    CheckpointFixture fixture;
    const auto database = fixture.directory / "queue.sqlite";
    const auto output = fixture.directory / "result.mkv";
    const std::string modelDigest(64,'a');
    FileIdentity sourceIdentity;
    {
        SourceLease lease(fixture.source);
        sourceIdentity = lease.fingerprint({});
    }
    ProcessingJob job;
    job.source = fixture.source.string();
    job.output = output.string();
    job.modelDirectory = fixture.directory / "model";
    job.runtimeDirectory = fixture.directory / "runtime";
    job.settings.targetRate = Rational(50);
    job.expectedModelId = "rife-4.25";
    job.expectedModelVersion = "4.25";
    job.expectedModelSha256 = modelDigest;
    auto oldConfiguration = nlohmann::json::parse(serializeJob(job));
    oldConfiguration["schemaVersion"] = 1;
    oldConfiguration.erase("expectedModelAdapterId");
    oldConfiguration.erase("expectedModelAdapterVersion");
    oldConfiguration.erase("restorationModelDirectory");
    oldConfiguration.erase("expectedRestorationDescriptorSha256");
    oldConfiguration.erase("temporalRestorationModelDirectory");
    oldConfiguration.erase("expectedTemporalRestorationDescriptorSha256");
    oldConfiguration.erase("automaticTemporalDenoise");
    auto manifestJob = oldConfiguration;
    manifestJob["expectedSourceSha256"] = sourceIdentity.sha256;
    const nlohmann::json identity{
        {"schemaVersion",1},
        {"source",{{"size",sourceIdentity.size},{"modified",sourceIdentity.modified},{"fileId",sourceIdentity.fileId},{"sha256",sourceIdentity.sha256}}},
        {"model",{{"id",job.expectedModelId},{"version",job.expectedModelVersion},{"sha256",modelDigest},{"inferenceBackend","onnxruntime_cuda"}}},
        {"device",{{"ordinal",0},{"name","fixture GPU"},{"pciBusId",""},{"computeMajor",7},{"computeMinor",5},{"driverCudaApiVersion",12020}}}};
    const auto oldManifest = nlohmann::json{
        {"schemaVersion",1},{"source",job.source},{"output",job.output},{"job",manifestJob},{"identity",identity},
        {"mediaRuntime",media::MediaEngine::runtimeVersion()},{"framesPerSegment",256}}.dump();
    std::string taskId;
    {
        TaskStore store(database.string());
        taskId = store.createConfigured({{job.source,job.output,oldConfiguration.dump()}}).front();
        store.transition(taskId,TaskState::running);
        store.saveIdentity(taskId,identity.dump());
        CheckpointArtifacts artifacts(store,taskId,output);
        artifacts.setManifest(oldManifest);
        const auto segment = createSegment(fixture,artifacts);
        (void)artifacts.commit(0,0,2,segment);
        EXPECT_EQ(store.recoverConfigured(),1U);
        EXPECT_EQ(store.checkpointManifest(taskId),oldManifest);
    }
    {
        TaskQueue queue(database);
        try { queue.resumeCheckpoint(taskId); FAIL() << "Legacy version-only media proof was accepted"; }
        catch (const std::logic_error& error) { EXPECT_NE(std::string(error.what()).find("no content-verified FFmpeg/writer identity"),std::string::npos); }
        const auto tasks = queue.list();
        ASSERT_EQ(tasks.size(),1U);
        EXPECT_EQ(tasks.front().task.state,TaskState::interrupted);
        EXPECT_EQ(tasks.front().configuration,oldConfiguration.dump());
        EXPECT_EQ(tasks.front().identity,identity.dump());
        EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),oldManifest);
        TaskStore verifyStore(database.string());
        CheckpointArtifacts artifacts(verifyStore,taskId,output);
        const auto preserved = artifacts.verifyCommitted();
        ASSERT_EQ(preserved.size(),1U);
        EXPECT_EQ(preserved.front().contract.frameCount,2U);
    }
}

TEST(CheckpointArtifacts, HashesAndJournalsOnlyVerifiedTaskOwnedFiles) {
    CheckpointFixture fixture;
    const auto output = fixture.directory / "result.mkv";
    rifelove::TaskStore store((fixture.directory / "queue.sqlite").string());
    const auto id = store.createConfigured({{fixture.source.string(),output.string(),"{}"}}).front();
    store.transition(id,rifelove::TaskState::running);
    rifelove::application::CheckpointArtifacts artifacts(store,id,output);
    artifacts.setManifest("{\"schemaVersion\":1,\"source\":\"test\"}");
    const auto segment = createSegment(fixture,artifacts);
    const auto record = artifacts.commit(0,0,2,segment);
    EXPECT_EQ(record.segmentIndex,0U);
    EXPECT_EQ(record.firstOutputFrame,0U);
    EXPECT_EQ(record.endOutputFrame,2U);
    EXPECT_TRUE(record.artifactPath.starts_with("segment-000000-"));
    EXPECT_EQ(record.sha256.size(),64U);
    const auto restored = artifacts.verifyCommitted();
    ASSERT_EQ(restored.size(),1U);
    EXPECT_EQ(restored.front().path,segment.path);
    EXPECT_EQ(restored.front().contract.firstTimestamp,rifelove::Rational(0));

    {
        std::ofstream damaged(segment.path,std::ios::binary | std::ios::app);
        damaged.put('x');
    }
    EXPECT_THROW((void)artifacts.verifyCommitted(),std::runtime_error);
}

#include "application/checkpoint_artifacts.hpp"
#include "application/job_serialization.hpp"
#include "application/task_queue.hpp"
#include "media/video_writer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace {
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
    current["job"]["schemaVersion"] = 3;
    current["job"]["inferenceBackend"] = "auto";
    current["job"]["expectedModelAdapterId"] = "rife-sdr-v1";
    current["job"]["expectedModelAdapterVersion"] = 1;
    current["identity"]["schemaVersion"] = 2;
    current["identity"]["model"]["adapter"] = {{"id","rife-sdr-v1"},{"version",1}};
    EXPECT_TRUE(rifelove::application::CheckpointArtifacts::manifestMatches(legacy.dump(),current.dump()));

    Json versionTwo = current;
    versionTwo["schemaVersion"] = 2;
    versionTwo["job"]["schemaVersion"] = 2;
    versionTwo["job"].erase("inferenceBackend");
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
}

TEST(CheckpointArtifacts, TaskQueueResumesLegacyManifestWithoutRewritingIt) {
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
        EXPECT_NO_THROW(queue.resumeCheckpoint(taskId));
        const auto tasks = queue.list();
        ASSERT_EQ(tasks.size(),1U);
        EXPECT_EQ(tasks.front().task.state,TaskState::queued);
        EXPECT_EQ(TaskStore(database.string()).checkpointManifest(taskId),oldManifest);
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

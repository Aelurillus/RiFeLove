#include "application/job_serialization.hpp"
#include "application/local_lease.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <random>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace rifelove;
using namespace rifelove::application;
ProcessingJob configuredJob() {
    ProcessingJob job; job.source = "C:/recordings/source.mkv"; job.output = "C:/exports/result.mkv";
    job.modelDirectory = L"C:/models/local"; job.runtimeDirectory = L"C:/runtime";
    job.settings.targetRate = Rational(60000,1001); job.deviceOrdinal = 1;
    job.sourceDurationEstimate = Rational(1001,24000);
    job.settings.hostFloatBufferLimit = 256 * 1024 * 1024;
    job.expectedModelId = "rife-4.25"; job.expectedModelVersion = "4.25"; job.expectedModelSha256 = std::string(64,'a'); job.expectedSourceSha256 = std::string(64,'b');
    auto& e = job.settings.encoding; e.encoder = "hevc_nvenc"; e.pixelFormat = "p010le"; e.videoStreamIndex = 2;
    e.timeBase = Rational(1,120000); e.frameRate = job.settings.targetRate; e.bitRate = 12345678; e.gopSize = 48; e.maxBFrames = 3;
    e.encoderOptions = {{"cq","20"},{"preset","p5"}};
    job.settings.scene = {0.2,0.3,0.6,1e-4}; return job;
}
}
TEST(ProcessingConfiguration, RoundTripsAllAdvancedSettingsAndPinnedIdentity) {
    const auto job = configuredJob(); const auto encoded = serializeJob(job);
    const auto restored = parseJob(job.source,job.output,encoded);
    EXPECT_EQ(restored.source,job.source); EXPECT_EQ(restored.output,job.output);
    EXPECT_EQ(serializeJob(restored),encoded);
    EXPECT_EQ(restored.settings.targetRate,Rational(60000,1001)); EXPECT_EQ(restored.settings.encoding.timeBase,Rational(1,120000));
    EXPECT_EQ(restored.sourceDurationEstimate,job.sourceDurationEstimate);
    EXPECT_EQ(restored.settings.encoding.encoderOptions.at("preset"),"p5"); EXPECT_EQ(restored.settings.encoding.maxBFrames,3);
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
    auto bad = job; bad.expectedSourceSha256 = "unverified"; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.scene.cutMeanDifference = -0.1; EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
    bad = job; bad.settings.encoding.frameRate = Rational(24); EXPECT_THROW((void)serializeJob(bad),std::invalid_argument);
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
}
TEST(ProcessingReport, SerializesRuntimeIdentityQualityDiagnosticsAndTimingEvidence) {
    PipelineResult result{};
    result.video.encoder = "libopenh264"; result.video.framesSubmitted = 120; result.video.videoPackets = 120; result.video.copiedPackets = 48;
    result.video.firstTimestamp = Rational(3,2); result.video.endExclusive = Rational(7,2);
    result.sourceFrames = 60; result.aiFrames = 60; result.originalFrames = 60; result.heldFrames = 0;
    result.sceneCuts = 1; result.duplicatePairs = 2; result.abnormalPairs = 3; result.recoveredTimestamps = 4;
    result.modelId = "rife-4.25-fp32-sdr8"; result.backend = "onnxruntime_tensorrt_cuda";
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
    EXPECT_EQ(report.at("performance").at("stageTimings").at("inferenceWaitSeconds"),4.5);
    EXPECT_EQ(report.at("performance").at("stageTimings").at("frameEncodeSeconds"),0.75);
    EXPECT_EQ(report.at("qualityDiagnostics").at("sceneCuts"),1);
    EXPECT_EQ(report.at("qualityDiagnostics").at("perceptualMetricsMeasured"),false);
    EXPECT_EQ(report.at("source").at("recoveredTimestamps"),4);
    EXPECT_EQ(report.at("timingValidation").at("timeBase"),"1/120");
    EXPECT_EQ(report.at("pipelineResult").at("abnormalPairs"),3);
    EXPECT_EQ(report.at("pipelineResult").at("timings").at("sourceDecodeSeconds"),1.25);
    EXPECT_EQ(report.at("pipelineResult").at("recoveredTimestamps"),4);
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

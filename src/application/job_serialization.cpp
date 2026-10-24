#include "application/job_serialization.hpp"
#include "inference/model_pack.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <type_traits>
#include <system_error>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::application {
namespace {
using Json = nlohmann::json;
std::string utf8(const std::filesystem::path& path) { const auto value = path.u8string(); return {value.begin(),value.end()}; }
std::filesystem::path path(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(),value.end())); }
void hash(const std::string& value, bool required = false) {
    if (!required && value.empty()) return;
    if (value.size() != 64 || !std::all_of(value.begin(),value.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Expected a lowercase SHA-256 value");
}
void text(const std::string& value, std::size_t limit, bool required = true) {
    if ((required && value.empty()) || value.size() > limit || value.find('\0') != std::string::npos) throw std::invalid_argument("Invalid task text field");
}
void threshold(double value) { if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Quality thresholds must be finite values from zero to one"); }
Json optional(const std::optional<Rational>& value) { return value ? Json(value->toString()) : Json(nullptr); }
std::optional<Rational> rational(const Json& value) { if (value.is_null()) return {}; return Rational::parse(value.get<std::string>()); }
template<class T> T integer(const Json& value) {
    if (!value.is_number_integer()) throw std::invalid_argument("Expected an integer task field");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) throw std::invalid_argument("Task integer is out of range");
        return static_cast<T>(number);
    }
    const auto number = value.get<std::int64_t>();
    if constexpr (std::is_unsigned_v<T>) {
        if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<T>::max()) throw std::invalid_argument("Task integer is out of range");
    } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) throw std::invalid_argument("Task integer is out of range");
    return static_cast<T>(number);
}
void validate(const ProcessingJob& job) {
    text(job.source,32768); text(job.output,32768); text(utf8(job.modelDirectory),32768); text(utf8(job.runtimeDirectory),32768);
    if (job.deviceOrdinal < 0 || job.settings.targetRate <= Rational(0) || !job.settings.hostFloatBufferLimit) throw std::invalid_argument("Invalid target/device/buffer configuration");
    if (job.sourceDurationEstimate && *job.sourceDurationEstimate <= Rational(0)) throw std::invalid_argument("Invalid source duration estimate");
    hash(job.expectedSourceSha256); hash(job.expectedModelSha256);
    text(job.expectedModelId,256,false); text(job.expectedModelVersion,256,false);
    const auto& e = job.settings.encoding;
    text(e.encoder,256); text(e.pixelFormat,256,false);
    if (e.videoStreamIndex < -1 || e.bitRate < 0 || e.gopSize < 0 || e.maxBFrames < 0 || e.maxBFrames > 16 ||
        (e.timeBase && *e.timeBase <= Rational(0)) || (e.frameRate && *e.frameRate <= Rational(0)) || e.encoderOptions.size() > 128)
        throw std::invalid_argument("Invalid video encoder configuration");
    if (e.frameRate && *e.frameRate != job.settings.targetRate) throw std::invalid_argument("Encoder frame rate must match the processing target rate");
    for (const auto& [key,value] : e.encoderOptions) { text(key,256); text(value,4096,false); }
    const auto& s = job.settings.scene;
    threshold(s.cutMeanDifference); threshold(s.cutHistogramDistance); threshold(s.abnormalMeanDifference); threshold(s.duplicateMaximumDifference);
}
}
std::string serializeJob(const ProcessingJob& job) {
    validate(job);
    const auto& e = job.settings.encoding; const auto& s = job.settings.scene;
    Json json{{"schemaVersion",1},{"targetRate",job.settings.targetRate.toString()},{"sourceDurationEstimate",optional(job.sourceDurationEstimate)},{"modelDirectory",utf8(job.modelDirectory)},
        {"runtimeDirectory",utf8(job.runtimeDirectory)},{"deviceOrdinal",job.deviceOrdinal},{"hostFloatBufferLimit",job.settings.hostFloatBufferLimit},
        {"expectedSourceSha256",job.expectedSourceSha256},{"expectedModelSha256",job.expectedModelSha256},{"expectedModelId",job.expectedModelId},{"expectedModelVersion",job.expectedModelVersion},
        {"encoding",{{"encoder",e.encoder},{"pixelFormat",e.pixelFormat},{"videoStreamIndex",e.videoStreamIndex},{"timeBase",optional(e.timeBase)},{"frameRate",optional(e.frameRate)},
            {"bitRate",e.bitRate},{"gopSize",e.gopSize},{"maxBFrames",e.maxBFrames},{"allowPrecisionLoss",e.allowPrecisionLoss},{"options",e.encoderOptions}}},
        {"scene",{{"cutMeanDifference",s.cutMeanDifference},{"cutHistogramDistance",s.cutHistogramDistance},{"abnormalMeanDifference",s.abnormalMeanDifference},{"duplicateMaximumDifference",s.duplicateMaximumDifference}}}};
    const auto result = json.dump(); if (result.size() > 64 * 1024) throw std::invalid_argument("Task configuration exceeds 64 KiB"); return result;
}
ProcessingJob parseJob(const std::string& source, const std::string& output, const std::string& value) {
    if (value.size() > 64 * 1024) throw std::invalid_argument("Task configuration exceeds 64 KiB");
    const auto json = Json::parse(value);
    if (json.at("schemaVersion") != 1) throw std::invalid_argument("Unsupported task configuration version");
    ProcessingJob job; job.source = source; job.output = output;
    job.settings.targetRate = Rational::parse(json.at("targetRate").get<std::string>());
    if (json.contains("sourceDurationEstimate")) job.sourceDurationEstimate = rational(json.at("sourceDurationEstimate"));
    job.modelDirectory = path(json.at("modelDirectory").get<std::string>()); job.runtimeDirectory = path(json.at("runtimeDirectory").get<std::string>());
    job.deviceOrdinal = integer<int>(json.at("deviceOrdinal")); job.settings.hostFloatBufferLimit = integer<std::size_t>(json.at("hostFloatBufferLimit"));
    job.expectedSourceSha256 = json.at("expectedSourceSha256").get<std::string>(); job.expectedModelSha256 = json.at("expectedModelSha256").get<std::string>();
    job.expectedModelId = json.at("expectedModelId").get<std::string>(); job.expectedModelVersion = json.at("expectedModelVersion").get<std::string>();
    const auto& encoded = json.at("encoding"); auto& e = job.settings.encoding;
    e.encoder = encoded.at("encoder").get<std::string>(); e.pixelFormat = encoded.at("pixelFormat").get<std::string>();
    e.videoStreamIndex = integer<int>(encoded.at("videoStreamIndex")); e.timeBase = rational(encoded.at("timeBase")); e.frameRate = rational(encoded.at("frameRate"));
    e.bitRate = integer<std::int64_t>(encoded.at("bitRate")); e.gopSize = integer<int>(encoded.at("gopSize")); e.maxBFrames = integer<int>(encoded.at("maxBFrames"));
    e.allowPrecisionLoss = encoded.at("allowPrecisionLoss").get<bool>(); e.encoderOptions = encoded.at("options").get<media::Metadata>();
    const auto& scene = json.at("scene"); auto& s = job.settings.scene;
    s.cutMeanDifference = scene.at("cutMeanDifference").get<double>(); s.cutHistogramDistance = scene.at("cutHistogramDistance").get<double>();
    s.abnormalMeanDifference = scene.at("abnormalMeanDifference").get<double>(); s.duplicateMaximumDifference = scene.at("duplicateMaximumDifference").get<double>();
    validate(job); return job;
}
void pinModelDescriptor(ProcessingJob& job) {
    const auto bytes = inference::readLocalFile(job.modelDirectory / "model.json",256 * 1024);
    if (bytes.empty()) throw std::invalid_argument("Model descriptor is empty");
    const auto json = Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    const auto descriptorVersion = json.at("schemaVersion").get<int>();
    if ((descriptorVersion != 1 && descriptorVersion != 2) || json.at("family") != "rife")
        throw std::invalid_argument("Unsupported model descriptor");
    const auto digest = json.at("sha256").get<std::string>(); hash(digest,true);
    const auto id = json.at("id").get<std::string>(), version = json.at("version").get<std::string>(); text(id,256); text(version,256);
    if ((!job.expectedModelSha256.empty() && job.expectedModelSha256 != digest) || (!job.expectedModelId.empty() && job.expectedModelId != id) ||
        (!job.expectedModelVersion.empty() && job.expectedModelVersion != version)) throw std::invalid_argument("Selected model differs from the pinned task descriptor");
    job.expectedModelSha256 = digest; job.expectedModelId = id; job.expectedModelVersion = version;
}
std::string serializeIdentity(const ProcessingIdentity& i) {
    return Json{{"schemaVersion",1},{"source",{{"size",i.source.size},{"modified",i.source.modified},{"fileId",i.source.fileId},{"sha256",i.source.sha256}}},
        {"model",{{"id",i.modelId},{"version",i.modelVersion},{"sha256",i.modelSha256},{"inferenceBackend",i.backend}}},
        {"device",{{"ordinal",i.device.ordinal},{"name",i.device.name},{"pciBusId",i.device.pciBusId},{"computeMajor",i.device.computeMajor},{"computeMinor",i.device.computeMinor},{"driverCudaApiVersion",i.device.driverCudaApiVersion}}}}.dump();
}
void applyStoredIdentity(ProcessingJob& job, const std::string& value) {
    if (value.empty()) return;
    const auto json = Json::parse(value);
    if (json.at("schemaVersion") != 1) throw std::invalid_argument("Unsupported task identity version");
    const auto source = json.at("source").at("sha256").get<std::string>(); hash(source,true);
    const auto model = json.at("model").at("sha256").get<std::string>(); hash(model,true);
    if ((!job.expectedSourceSha256.empty() && job.expectedSourceSha256 != source) || (!job.expectedModelSha256.empty() && job.expectedModelSha256 != model)) throw std::invalid_argument("Stored identity contradicts task configuration");
    job.expectedSourceSha256 = source; job.expectedModelSha256 = model;
}
std::string serializeProgress(const ProcessingEvent& event) {
    return Json{{"schemaVersion",1},{"stage",event.stage},{"frames",event.outputFrames},{"aiFrames",event.aiFrames},{"processingFps",event.processingFps},{"timestamp",optional(event.timestamp)}}.dump();
}
std::string serializeResult(const PipelineResult& r) {
    return Json{{"schemaVersion",1},{"modelId",r.modelId},{"backend",r.backend},{"inferenceFallbackReason",r.inferenceFallbackReason},
        {"encoder",r.video.encoder},{"sourceFrames",r.sourceFrames},{"outputFrames",r.video.framesSubmitted},
        {"aiFrames",r.aiFrames},{"originalFrames",r.originalFrames},{"heldFrames",r.heldFrames},{"sceneCuts",r.sceneCuts},{"duplicatePairs",r.duplicatePairs},{"abnormalPairs",r.abnormalPairs},
        {"firstTimestamp",r.video.firstTimestamp.toString()},{"endExclusive",r.video.endExclusive.toString()},{"elapsedSeconds",r.elapsedSeconds},{"processingFps",r.processingFps},{"encoderFallbackReason",r.encoderFallbackReason},
        {"validation",{{"decodedFrames",r.validation.decodedFrames},{"timeBase",r.validation.timeBase.toString()},{"maximumTimestampError",r.validation.maximumTimestampError.toString()},
            {"durationError",r.validation.durationError.toString()},{"declaredEndError",optional(r.validation.declaredEndError)},{"auxiliaryMaximumEndError",r.validation.auxiliaryMaximumEndError.toString()},{"quantized",r.validation.quantized}}}}.dump();
}
std::string serializeProcessingReport(const ProcessingReportContext& context, const PipelineResult& result) {
    if (context.sourcePath.empty() || context.outputPath.empty() || context.modelVersion.empty() || context.modelSha256.size() != 64)
        throw std::invalid_argument("Processing report is missing source, output or verified model identity");
    if (Rational::parse(context.targetFrameRate) <= Rational(0)) throw std::invalid_argument("Processing report frame rate must be positive");
    if (!std::all_of(context.modelSha256.begin(),context.modelSha256.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Processing report model digest must be lowercase SHA-256");
    if (!std::isfinite(context.endToEndElapsedSeconds) || context.endToEndElapsedSeconds <= 0 ||
        !std::isfinite(result.elapsedSeconds) || result.elapsedSeconds <= 0 || !std::isfinite(result.processingFps) || result.processingFps < 0)
        throw std::invalid_argument("Processing report contains invalid performance measurements");

    const auto& device = context.device;
    const auto& encoding = context.encoding;
    const auto pipeline = Json::parse(serializeResult(result));
    const Json report{
        {"schemaVersion",1},
        {"application",{{"name","RifeLove"},{"ffmpegRuntime",media::MediaEngine::runtimeVersion()}}},
        {"source",{{"path",context.sourcePath},{"decodedFrames",result.sourceFrames},{"endEvidence",result.sourceEndEvidence}}},
        {"output",{{"path",context.outputPath},{"targetFrameRate",context.targetFrameRate},{"encoder",result.video.encoder},
            {"submittedFrames",result.video.framesSubmitted},{"videoPackets",result.video.videoPackets},{"copiedPackets",result.video.copiedPackets},
            {"firstTimestamp",result.video.firstTimestamp.toString()},{"endExclusive",result.video.endExclusive.toString()}}},
        {"model",{{"id",result.modelId},{"version",context.modelVersion},{"sha256",context.modelSha256},{"backend",result.backend},
            {"fallbackReason",result.inferenceFallbackReason}}},
        {"gpu",{{"ordinal",device.ordinal},{"name",device.name},{"pciBusId",device.pciBusId},
            {"computeCapability",std::to_string(device.computeMajor) + "." + std::to_string(device.computeMinor)},
            {"driverCudaApiVersion",device.driverCudaApiVersion},{"totalVramBytes",device.totalBytes}}},
        {"encoding",{{"requestedEncoder",encoding.encoder},{"actualEncoder",result.video.encoder},{"pixelFormat",encoding.pixelFormat},
            {"bitRate",encoding.bitRate},{"gopSize",encoding.gopSize},{"maxBFrames",encoding.maxBFrames},
            {"options",encoding.encoderOptions},{"fallbackReason",result.encoderFallbackReason}}},
        {"performance",{{"elapsedSeconds",context.endToEndElapsedSeconds},
            {"endToEndOutputFramesPerSecond",static_cast<double>(result.video.framesSubmitted) / context.endToEndElapsedSeconds},
            {"pipelineOutputFramesPerSecond",result.processingFps},
            {"aiFrames",result.aiFrames},{"originalFrames",result.originalFrames},{"heldFrames",result.heldFrames}}},
        {"qualityDiagnostics",{{"sceneCuts",result.sceneCuts},{"duplicatePairs",result.duplicatePairs},{"abnormalMotionPairs",result.abnormalPairs},
            {"perceptualMetricsMeasured",false},
            {"note","No reference video was supplied; PSNR, SSIM and LPIPS were not measured."}}},
        {"timingValidation",pipeline.at("validation")},
        {"pipelineResult",pipeline}
    };
    return report.dump(2) + "\n";
}
ProcessingReportFile::ProcessingReportFile(std::filesystem::path destination) : destination_(std::move(destination)) {
    if (std::filesystem::exists(destination_)) throw std::invalid_argument("Processing report already exists; choose a new path");
    std::random_device random;
    for (int attempt = 0; attempt < 10; ++attempt) {
        temporary_ = destination_;
        temporary_ += ".rifelove-" + std::to_string(random()) + "-" + std::to_string(random()) + ".partial";
        HANDLE file = CreateFileW(temporary_.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return; }
        const auto code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
            throw std::system_error(static_cast<int>(code),std::system_category(),"reserve processing report");
    }
    throw std::runtime_error("Could not reserve a temporary processing report file");
}
void ProcessingReportFile::publish(std::string_view serializedReport) {
    if (published_) throw std::logic_error("Processing report has already been published");
    if (serializedReport.empty() || serializedReport.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::invalid_argument("Processing report is empty or too large");
    {
        std::ofstream file(temporary_,std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Could not open temporary processing report");
        file.write(serializedReport.data(),static_cast<std::streamsize>(serializedReport.size()));
        file.flush();
        if (!file) throw std::runtime_error("Could not write temporary processing report");
    }
    if (!MoveFileExW(temporary_.c_str(),destination_.c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"publish processing report without replacement");
    published_ = true;
}
ProcessingReportFile::~ProcessingReportFile() {
    if (!published_) { std::error_code error; std::filesystem::remove(temporary_,error); }
}
} // namespace rifelove::application

#include "application/job_serialization.hpp"
#include "inference/model_pack.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/stdf_tile_policy.hpp"
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
bool adapterIdentifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
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
std::pair<std::string,int> descriptorAdapter(const Json& descriptor) {
    const auto found = descriptor.find("adapter");
    if (found == descriptor.end()) return {"rife-sdr-v1",1};
    if (!found->is_object() || !found->contains("id") || !found->at("id").is_string())
        throw std::invalid_argument("Model adapter metadata is missing or invalid");
    const auto id = found->at("id").get<std::string>();
    if (!adapterIdentifier(id) || !found->contains("version") || !found->at("version").is_number_integer())
        throw std::invalid_argument("Model adapter identity is invalid");
    const auto version = integer<int>(found->at("version"));
    if (version <= 0) throw std::invalid_argument("Model adapter version is invalid");
    return {id,version};
}
std::pair<std::string,int> identityAdapter(const Json& identity) {
    const auto schema = identity.at("schemaVersion").get<int>();
    if (schema == 1) {
        if (identity.at("model").contains("adapter"))
            throw std::invalid_argument("Legacy task identity cannot declare a model adapter");
        return {"rife-sdr-v1",1};
    }
    if (schema != 2) throw std::invalid_argument("Unsupported task identity version");
    const auto& adapter = identity.at("model").at("adapter");
    if (!adapter.is_object() || !adapter.contains("id") || !adapter.at("id").is_string() ||
        !adapterIdentifier(adapter.at("id").get<std::string>()))
        throw std::invalid_argument("Stored model adapter identity is invalid");
    const auto version = integer<int>(adapter.at("version"));
    if (version <= 0) throw std::invalid_argument("Stored model adapter version is invalid");
    return {adapter.at("id").get<std::string>(),version};
}
void threshold(double value) { if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("Quality thresholds must be finite values from zero to one"); }
void temporalDenoiseSigma(float value) {
    if (!std::isfinite(value) || value < 0.0F || value > 55.0F || (value > 0.0F && value < 5.0F))
        throw std::invalid_argument("Temporal restoration noise sigma must be zero or a value from 5 to 55");
}
Json optional(const std::optional<Rational>& value) { return value ? Json(value->toString()) : Json(nullptr); }
std::optional<Rational> rational(const Json& value) { if (value.is_null()) return {}; return Rational::parse(value.get<std::string>()); }
Json timingBreakdown(const PipelineTimingBreakdown& value) {
    return { {"sourceProbeSeconds",value.sourceProbeSeconds}, {"encoderSetupSeconds",value.encoderSetupSeconds},
        {"sourceDecodeSeconds",value.sourceDecodeSeconds}, {"sourcePreparationSeconds",value.sourcePreparationSeconds},
        {"sceneAnalysisSeconds",value.sceneAnalysisSeconds}, {"inputUploadSeconds",value.inputUploadSeconds},
        {"inferenceWaitSeconds",value.inferenceWaitSeconds}, {"outputDownloadSeconds",value.outputDownloadSeconds},
        {"colorRestoreSeconds",value.colorRestoreSeconds}, {"restorationSeconds",value.restorationSeconds}, {"frameEncodeSeconds",value.frameEncodeSeconds},
        {"outputFinalizeSeconds",value.outputFinalizeSeconds}, {"outputValidationSeconds",value.outputValidationSeconds},
        {"pipelineWallSeconds",value.pipelineWallSeconds} };
}
std::pair<std::size_t,std::size_t> processingCounters(const PipelineResult& result) {
    const auto outputFrames = result.video.framesSubmitted;
    // Results created by older callers do not have the two attempt counters.
    // Treat those as a non-resumed run so their serialized meaning remains
    // unchanged. A complete resume is represented by reusedFrames > 0 and
    // therefore does not take this compatibility path.
    auto processed = result.processedFrames;
    auto reused = result.reusedFrames;
    if (processed == 0 && reused == 0 && outputFrames != 0) processed = outputFrames;
    if (processed > outputFrames || reused > outputFrames - processed)
        throw std::invalid_argument("Processing frame counters exceed the output timeline");
    return {processed,reused};
}
std::pair<std::size_t,std::size_t> processingCounters(const ProcessingEvent& event) {
    auto processed = event.processedFrames;
    auto reused = event.reusedFrames;
    if (processed > event.outputFrames || reused > event.outputFrames - processed)
        throw std::invalid_argument("Progress frame counters exceed the output timeline");
    return {processed,reused};
}
void validate(const ProcessingJob& job) {
    text(job.source,32768); text(job.output,32768); text(utf8(job.modelDirectory),32768); text(utf8(job.runtimeDirectory),32768);
    text(utf8(job.restorationModelDirectory),32768,false);
    text(utf8(job.temporalRestorationModelDirectory),32768,false);
    text(utf8(job.compressionRestorationModelDirectory),32768,false);
    if (job.deviceOrdinal < 0 || job.settings.targetRate <= Rational(0) || !job.settings.hostFloatBufferLimit) throw std::invalid_argument("Invalid target/device/buffer configuration");
    if (job.sourceDurationEstimate && *job.sourceDurationEstimate <= Rational(0)) throw std::invalid_argument("Invalid source duration estimate");
    hash(job.expectedSourceSha256); hash(job.expectedModelSha256);
    hash(job.expectedRestorationDescriptorSha256);
    hash(job.expectedTemporalRestorationDescriptorSha256);
    hash(job.expectedCompressionRestorationDescriptorSha256);
    if (job.restorationModelDirectory.empty() != job.expectedRestorationDescriptorSha256.empty())
        throw std::invalid_argument("A local restoration model path and pinned descriptor hash must be supplied together");
    if (job.temporalRestorationModelDirectory.empty() != job.expectedTemporalRestorationDescriptorSha256.empty())
        throw std::invalid_argument("A temporal restoration model path and pinned descriptor hash must be supplied together");
    if (!std::isfinite(job.compressionRestorationStrength) || job.compressionRestorationStrength < 0.0F ||
        job.compressionRestorationStrength > 1.0F)
        throw std::invalid_argument("Compression restoration strength must be a finite value from zero to one");
    const bool compressionDisabled = job.compressionRestorationModelDirectory.empty() &&
        job.expectedCompressionRestorationDescriptorSha256.empty() && job.compressionRestorationStrength == 0.0F;
    const bool compressionEnabled = !job.compressionRestorationModelDirectory.empty() &&
        !job.expectedCompressionRestorationDescriptorSha256.empty() && job.compressionRestorationStrength > 0.0F;
    if (!compressionDisabled && !compressionEnabled)
        throw std::invalid_argument("Compression restoration directory, descriptor hash and strength must be enabled together");
    if (job.compressionRestorationTiled && !compressionEnabled)
        throw std::invalid_argument("Tiled compression restoration requires an enabled, pinned STDF model and positive strength");
    if (job.settings.automaticCompressionRestoration) {
        if (!compressionEnabled)
            throw std::invalid_argument("Automatic compression restoration requires an enabled, pinned STDF model");
        (void)restoration::AutomaticCompressionController(*job.settings.automaticCompressionRestoration);
    }
    temporalDenoiseSigma(job.settings.temporalDenoiseSigma255);
    if (job.settings.automaticTemporalDenoise) {
        (void)restoration::AutomaticDenoiseController(*job.settings.automaticTemporalDenoise);
    }
    if (job.settings.motionBlur.shutterAngleDegrees < 0 || job.settings.motionBlur.shutterAngleDegrees > 360 ||
        job.settings.motionBlur.sampleCount < 1 || job.settings.motionBlur.sampleCount > 16)
        throw std::invalid_argument("Motion blur shutter angle must be 0..360 degrees and sample count must be 1..16");
    text(job.expectedModelId,256,false); text(job.expectedModelVersion,256,false);
    if (job.expectedModelAdapterId.empty()) {
        if (job.expectedModelAdapterVersion != 0) throw std::invalid_argument("An unpinned model adapter must use version zero");
    } else if (!adapterIdentifier(job.expectedModelAdapterId) || job.expectedModelAdapterVersion <= 0) {
        throw std::invalid_argument("Invalid pinned model adapter identity");
    }
    const auto& e = job.settings.encoding;
    text(e.encoder,256); text(e.pixelFormat,256,false);
    if (e.videoStreamIndex < -1 || e.bitRate < 0 || e.gopSize < 0 || e.maxBFrames < 0 || e.maxBFrames > 16 ||
        (e.outputWidth == 0) != (e.outputHeight == 0) || e.outputWidth < 0 || e.outputHeight < 0 ||
        e.outputWidth > 16384 || e.outputHeight > 16384 ||
        (e.timeBase && *e.timeBase <= Rational(0)) || (e.frameRate && *e.frameRate <= Rational(0)) || e.encoderOptions.size() > 128)
        throw std::invalid_argument("Invalid video encoder configuration");
    if (e.frameRate && *e.frameRate != job.settings.targetRate) throw std::invalid_argument("Encoder frame rate must match the processing target rate");
    for (const auto& [key,value] : e.encoderOptions) { text(key,256); text(value,4096,false); }
    const auto& s = job.settings.scene;
    threshold(s.cutMeanDifference); threshold(s.cutHistogramDistance); threshold(s.abnormalMeanDifference); threshold(s.duplicateMaximumDifference);
    if (job.settings.inferenceBackend != "auto" && job.settings.inferenceBackend != "cuda" && job.settings.inferenceBackend != "tensorrt")
        throw std::invalid_argument("Unsupported inference backend preference");
}
}
std::string serializeCompressionRestorationPolicy(bool tiled) {
    Json policy{{"mode",tiled ? "tiled" : "full-frame"},
        {"tilePolicy",tiled ? restoration::stdf_tiles::policyId : "full-frame-bounded-384x256-v1"},
        {"maximumInputWidth",384},{"maximumInputHeight",256}};
    if (tiled) {
        policy["maximumInputWidth"] = restoration::stdf_tiles::maximumInputWidth;
        policy["maximumInputHeight"] = restoration::stdf_tiles::maximumInputHeight;
        policy["coreWidth"] = restoration::stdf_tiles::coreWidth;
        policy["coreHeight"] = restoration::stdf_tiles::coreHeight;
        policy["halo"] = restoration::stdf_tiles::halo;
        policy["alignment"] = restoration::stdf_tiles::alignment;
        policy["edgePolicy"] = "global-right-bottom-pad-to8-clip-halo-v1";
        policy["maximumAxis"] = restoration::stdf_tiles::maximumAxis;
        policy["maximumPixels"] = restoration::stdf_tiles::maximumPixels;
    }
    return policy.dump();
}
bool supportsCompressionRestorationDimensions(bool tiled, int width, int height) noexcept {
    return tiled ? restoration::stdf_tiles::supportsDimensions(width,height)
        : width > 0 && height > 0 && width <= 384 && height <= 256;
}
bool compressionRestorationRuntimeMatchesPolicy(bool tiled, std::string_view signature, std::string_view graphSha256) noexcept {
    if (signature.empty()) return false;
    if (!tiled) return signature.find("tilePolicy-") == std::string_view::npos;
    if (graphSha256.size() != 64) return false;
    bool graphFound = false, policyFound = false;
    while (!signature.empty()) {
        const auto separator = signature.find(';');
        const auto token = signature.substr(0,separator);
        if (token.starts_with("graph-")) {
            if (graphFound || token.substr(6) != graphSha256) return false;
            graphFound = true;
        } else if (token.starts_with("tilePolicy-")) {
            if (policyFound || token.substr(11) != restoration::stdf_tiles::policyId) return false;
            policyFound = true;
        }
        if (separator == std::string_view::npos) break;
        signature.remove_prefix(separator + 1);
    }
    return graphFound && policyFound;
}
std::string serializeJob(const ProcessingJob& job) {
    validate(job);
    const auto& e = job.settings.encoding; const auto& s = job.settings.scene;
    Json json{{"schemaVersion",12},{"targetRate",job.settings.targetRate.toString()},{"sourceDurationEstimate",optional(job.sourceDurationEstimate)},{"modelDirectory",utf8(job.modelDirectory)},
        {"restorationModelDirectory",utf8(job.restorationModelDirectory)},
        {"expectedRestorationDescriptorSha256",job.expectedRestorationDescriptorSha256},
        {"temporalRestorationModelDirectory",utf8(job.temporalRestorationModelDirectory)},
        {"expectedTemporalRestorationDescriptorSha256",job.expectedTemporalRestorationDescriptorSha256},
        {"compressionRestorationModelDirectory",utf8(job.compressionRestorationModelDirectory)},
        {"expectedCompressionRestorationDescriptorSha256",job.expectedCompressionRestorationDescriptorSha256},
        {"compressionRestorationStrength",job.compressionRestorationStrength},
        {"compressionRestorationTiled",job.compressionRestorationTiled},
        {"compressionRestorationPolicy",Json::parse(serializeCompressionRestorationPolicy(job.compressionRestorationTiled))},
        {"runtimeDirectory",utf8(job.runtimeDirectory)},{"deviceOrdinal",job.deviceOrdinal},{"hostFloatBufferLimit",job.settings.hostFloatBufferLimit},
        {"inferenceBackend",job.settings.inferenceBackend},
        {"expectedSourceSha256",job.expectedSourceSha256},{"expectedModelSha256",job.expectedModelSha256},{"expectedModelId",job.expectedModelId},{"expectedModelVersion",job.expectedModelVersion},
        {"expectedModelAdapterId",job.expectedModelAdapterId},{"expectedModelAdapterVersion",job.expectedModelAdapterVersion},
        {"temporalDenoiseSigma255",job.settings.temporalDenoiseSigma255},
        {"automaticTemporalDenoise",nullptr},
        {"automaticCompressionRestoration",nullptr},
        {"motionBlur",{{"enabled",job.settings.motionBlur.enabled},{"shutterAngleDegrees",job.settings.motionBlur.shutterAngleDegrees},{"sampleCount",job.settings.motionBlur.sampleCount}}},
        {"encoding",{{"encoder",e.encoder},{"pixelFormat",e.pixelFormat},{"videoStreamIndex",e.videoStreamIndex},{"outputWidth",e.outputWidth},{"outputHeight",e.outputHeight},{"timeBase",optional(e.timeBase)},{"frameRate",optional(e.frameRate)},
            {"bitRate",e.bitRate},{"gopSize",e.gopSize},{"maxBFrames",e.maxBFrames},{"allowPrecisionLoss",e.allowPrecisionLoss},{"options",e.encoderOptions}}},
        {"scene",{{"cutMeanDifference",s.cutMeanDifference},{"cutHistogramDistance",s.cutHistogramDistance},{"abnormalMeanDifference",s.abnormalMeanDifference},{"duplicateMaximumDifference",s.duplicateMaximumDifference}}}};
    // The full-PTS shutter changes pixels compared with the former four-frame
    // window. Bind enabled jobs/checkpoints to this algorithm; old sharp
    // fallback segments must not be silently reused by the corrected path.
    if (job.settings.motionBlur.enabled) json["motionBlur"]["algorithmVersion"] = 2;
    if (job.settings.automaticTemporalDenoise) {
        const auto& automatic = *job.settings.automaticTemporalDenoise;
        json["automaticTemporalDenoise"] = {{"algorithmVersion",automatic.algorithmVersion},
            {"replayPolicy",restoration::automaticDenoiseReplayPolicy},{"relativeAdjustment",automatic.relativeAdjustment},
            {"smoothingSeconds",automatic.smoothingSeconds},{"maximumSigmaChangePerSecond",automatic.maximumSigmaChangePerSecond},
            {"minimumConfidence",automatic.minimumConfidence}};
    }
    if (job.settings.automaticCompressionRestoration) {
        const auto& automatic = *job.settings.automaticCompressionRestoration;
        json["automaticCompressionRestoration"] = {{"algorithmVersion",restoration::automaticCompressionAlgorithmVersion},
            {"analyzer",restoration::automaticCompressionAnalyzerId},{"replayPolicy",restoration::automaticCompressionReplayPolicy},
            {"relativeAdjustment",automatic.relativeAdjustment},{"smoothingSeconds",automatic.smoothingSeconds},
            {"maximumStrengthChangePerSecond",automatic.maximumStrengthChangePerSecond},{"minimumConfidence",automatic.minimumConfidence}};
    }
    const auto result = json.dump(); if (result.size() > 64 * 1024) throw std::invalid_argument("Task configuration exceeds 64 KiB"); return result;
}
ProcessingJob parseJob(const std::string& source, const std::string& output, const std::string& value) {
    if (value.size() > 64 * 1024) throw std::invalid_argument("Task configuration exceeds 64 KiB");
    const auto json = Json::parse(value);
    const auto schemaVersion = integer<int>(json.at("schemaVersion"));
    if (schemaVersion < 1 || schemaVersion > 12) throw std::invalid_argument("Unsupported task configuration version");
    ProcessingJob job; job.source = source; job.output = output;
    job.settings.targetRate = Rational::parse(json.at("targetRate").get<std::string>());
    if (json.contains("sourceDurationEstimate")) job.sourceDurationEstimate = rational(json.at("sourceDurationEstimate"));
    job.modelDirectory = path(json.at("modelDirectory").get<std::string>()); job.runtimeDirectory = path(json.at("runtimeDirectory").get<std::string>());
    if (schemaVersion >= 4) {
        job.restorationModelDirectory = path(json.at("restorationModelDirectory").get<std::string>());
        job.expectedRestorationDescriptorSha256 = json.at("expectedRestorationDescriptorSha256").get<std::string>();
    }
    if (schemaVersion >= 8) {
        job.temporalRestorationModelDirectory = path(json.at("temporalRestorationModelDirectory").get<std::string>());
        job.expectedTemporalRestorationDescriptorSha256 = json.at("expectedTemporalRestorationDescriptorSha256").get<std::string>();
    }
    if (schemaVersion >= 10) {
        job.compressionRestorationModelDirectory = path(json.at("compressionRestorationModelDirectory").get<std::string>());
        job.expectedCompressionRestorationDescriptorSha256 = json.at("expectedCompressionRestorationDescriptorSha256").get<std::string>();
        job.compressionRestorationStrength = json.at("compressionRestorationStrength").get<float>();
    }
    if (schemaVersion >= 11) {
        if (!json.at("compressionRestorationTiled").is_boolean())
            throw std::invalid_argument("Compression restoration tiling must be an explicit boolean");
        job.compressionRestorationTiled = json.at("compressionRestorationTiled").get<bool>();
        if (json.at("compressionRestorationPolicy").dump() != serializeCompressionRestorationPolicy(job.compressionRestorationTiled))
            throw std::invalid_argument("Unsupported compression restoration execution policy");
    } else {
        // Some callers lower a newly written configuration's schema to form a
        // historical disabled fixture. Only the exact full-frame defaults may
        // be ignored; a tiled flag/policy cannot be smuggled into schema 10.
        if ((json.contains("compressionRestorationTiled") &&
            (!json.at("compressionRestorationTiled").is_boolean() || json.at("compressionRestorationTiled").get<bool>())) ||
            (json.contains("compressionRestorationPolicy") &&
            json.at("compressionRestorationPolicy").dump() != serializeCompressionRestorationPolicy(false)))
            throw std::invalid_argument("Legacy task schemas cannot enable tiled compression restoration");
    }
    job.deviceOrdinal = integer<int>(json.at("deviceOrdinal")); job.settings.hostFloatBufferLimit = integer<std::size_t>(json.at("hostFloatBufferLimit"));
    if (schemaVersion >= 3) job.settings.inferenceBackend = json.at("inferenceBackend").get<std::string>();
    if (schemaVersion >= 7) job.settings.temporalDenoiseSigma255 = json.at("temporalDenoiseSigma255").get<float>();
    if (schemaVersion >= 12) {
        const auto& automatic = json.at("automaticCompressionRestoration");
        if (!automatic.is_null()) {
            if (!automatic.is_object() || automatic.size() != 7 ||
                integer<int>(automatic.at("algorithmVersion")) != restoration::automaticCompressionAlgorithmVersion ||
                automatic.at("analyzer").get<std::string>() != restoration::automaticCompressionAnalyzerId ||
                automatic.at("replayPolicy").get<std::string>() != restoration::automaticCompressionReplayPolicy)
                throw std::invalid_argument("Unsupported automatic compression-restoration algorithm or replay policy");
            job.settings.automaticCompressionRestoration = restoration::AutomaticCompressionSettings{
                automatic.at("relativeAdjustment").get<double>(),automatic.at("smoothingSeconds").get<double>(),
                automatic.at("maximumStrengthChangePerSecond").get<double>(),automatic.at("minimumConfidence").get<double>()};
        }
    } else if (json.contains("automaticCompressionRestoration") && !json.at("automaticCompressionRestoration").is_null()) {
        throw std::invalid_argument("Historical task schema cannot declare automatic compression restoration");
    }
    if (schemaVersion >= 9 && !json.at("automaticTemporalDenoise").is_null()) {
        const auto& automatic = json.at("automaticTemporalDenoise");
        if (!automatic.is_object() || automatic.size() != 6 ||
            (integer<int>(automatic.at("algorithmVersion")) != 1 && integer<int>(automatic.at("algorithmVersion")) != 2) ||
            automatic.at("replayPolicy").get<std::string>() != restoration::automaticDenoiseReplayPolicy)
            throw std::invalid_argument("Unsupported automatic Gaussian-denoise algorithm or replay policy");
        job.settings.automaticTemporalDenoise = restoration::AutomaticDenoiseSettings{
            automatic.at("relativeAdjustment").get<double>(),automatic.at("smoothingSeconds").get<double>(),
            automatic.at("maximumSigmaChangePerSecond").get<double>(),automatic.at("minimumConfidence").get<double>(),
            integer<int>(automatic.at("algorithmVersion"))};
    }
    if (schemaVersion >= 5) {
        const auto& blur = json.at("motionBlur");
        job.settings.motionBlur.enabled = blur.at("enabled").get<bool>();
        job.settings.motionBlur.shutterAngleDegrees = integer<int>(blur.at("shutterAngleDegrees"));
        job.settings.motionBlur.sampleCount = integer<int>(blur.at("sampleCount"));
        if (blur.contains("algorithmVersion") && integer<int>(blur.at("algorithmVersion")) != 2)
            throw std::invalid_argument("Unsupported motion blur algorithm version");
    }
    job.expectedSourceSha256 = json.at("expectedSourceSha256").get<std::string>(); job.expectedModelSha256 = json.at("expectedModelSha256").get<std::string>();
    job.expectedModelId = json.at("expectedModelId").get<std::string>(); job.expectedModelVersion = json.at("expectedModelVersion").get<std::string>();
    if (schemaVersion >= 2) {
        job.expectedModelAdapterId = json.at("expectedModelAdapterId").get<std::string>();
        job.expectedModelAdapterVersion = integer<int>(json.at("expectedModelAdapterVersion"));
    }
    const auto& encoded = json.at("encoding"); auto& e = job.settings.encoding;
    e.encoder = encoded.at("encoder").get<std::string>(); e.pixelFormat = encoded.at("pixelFormat").get<std::string>();
    e.videoStreamIndex = integer<int>(encoded.at("videoStreamIndex")); e.timeBase = rational(encoded.at("timeBase")); e.frameRate = rational(encoded.at("frameRate"));
    if (schemaVersion >= 6) {
        e.outputWidth = integer<int>(encoded.at("outputWidth"));
        e.outputHeight = integer<int>(encoded.at("outputHeight"));
    }
    e.bitRate = integer<std::int64_t>(encoded.at("bitRate")); e.gopSize = integer<int>(encoded.at("gopSize")); e.maxBFrames = integer<int>(encoded.at("maxBFrames"));
    e.allowPrecisionLoss = encoded.at("allowPrecisionLoss").get<bool>(); e.encoderOptions = encoded.at("options").get<media::Metadata>();
    const auto& scene = json.at("scene"); auto& s = job.settings.scene;
    s.cutMeanDifference = scene.at("cutMeanDifference").get<double>(); s.cutHistogramDistance = scene.at("cutHistogramDistance").get<double>();
    s.abnormalMeanDifference = scene.at("abnormalMeanDifference").get<double>(); s.duplicateMaximumDifference = scene.at("duplicateMaximumDifference").get<double>();
    validate(job); return job;
}
void pinModelDescriptor(ProcessingJob& job) {
    if (job.expectedModelAdapterId.empty() && job.expectedModelAdapterVersion != 0)
        throw std::invalid_argument("An unpinned model adapter must use version zero");
    const auto bytes = inference::readLocalFile(job.modelDirectory / "model.json",256 * 1024);
    if (bytes.empty()) throw std::invalid_argument("Model descriptor is empty");
    const auto json = Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    const auto descriptorVersion = json.at("schemaVersion").get<int>();
    if ((descriptorVersion != 1 && descriptorVersion != 2) || json.at("family") != "rife")
        throw std::invalid_argument("Unsupported model descriptor");
    const auto digest = json.at("sha256").get<std::string>(); hash(digest,true);
    const auto id = json.at("id").get<std::string>(), version = json.at("version").get<std::string>(); text(id,256); text(version,256);
    const auto [adapterId,adapterVersion] = descriptorAdapter(json);
    if ((!job.expectedModelSha256.empty() && job.expectedModelSha256 != digest) || (!job.expectedModelId.empty() && job.expectedModelId != id) ||
        (!job.expectedModelVersion.empty() && job.expectedModelVersion != version) ||
        (!job.expectedModelAdapterId.empty() && (job.expectedModelAdapterId != adapterId || job.expectedModelAdapterVersion != adapterVersion)))
        throw std::invalid_argument("Selected model differs from the pinned task descriptor");
    job.expectedModelSha256 = digest; job.expectedModelId = id; job.expectedModelVersion = version;
    job.expectedModelAdapterId = adapterId; job.expectedModelAdapterVersion = adapterVersion;
}
void pinRestorationModelDescriptor(ProcessingJob& job) {
    const auto pin = [](std::filesystem::path& directory, std::string& expected,
                        bool temporalOnly, bool imageOnly) {
        if (directory.empty()) {
            if (!expected.empty()) throw std::invalid_argument("A pinned local restoration model has no directory");
            return;
        }
        const auto model = restoration::loadModelPack(directory);
        const bool image = restoration::supportsOnnxCudaOutputRateModel(model);
        const bool temporal = restoration::supportsOnnxCudaTemporalModel(model);
        if ((!image && !temporal) || (temporalOnly && !temporal) || (imageOnly && !image))
            throw std::invalid_argument("The selected local restoration model is incompatible with its pipeline placement");
        const auto descriptorHash = inference::sha256File(model.directory / "model.json",256 * 1024);
        if (!expected.empty() && expected != descriptorHash)
            throw std::invalid_argument("Selected restoration model differs from its pinned local descriptor");
        directory = model.directory;
        expected = descriptorHash;
    };
    // The legacy field can still contain either kind of model. If a separate
    // temporal model is selected, the legacy field must be output-rate so the
    // chain cannot accidentally run two denoisers or two spatial models.
    auto imageDirectory = job.restorationModelDirectory;
    auto imageHash = job.expectedRestorationDescriptorSha256;
    auto temporalDirectory = job.temporalRestorationModelDirectory;
    auto temporalHash = job.expectedTemporalRestorationDescriptorSha256;
    pin(imageDirectory,imageHash,false,!temporalDirectory.empty());
    pin(temporalDirectory,temporalHash,true,false);
    job.restorationModelDirectory = std::move(imageDirectory);
    job.expectedRestorationDescriptorSha256 = std::move(imageHash);
    job.temporalRestorationModelDirectory = std::move(temporalDirectory);
    job.expectedTemporalRestorationDescriptorSha256 = std::move(temporalHash);

    if (!std::isfinite(job.compressionRestorationStrength) || job.compressionRestorationStrength < 0.0F ||
        job.compressionRestorationStrength > 1.0F)
        throw std::invalid_argument("Compression restoration strength must be a finite value from zero to one");
    if (job.compressionRestorationModelDirectory.empty()) {
        if (!job.expectedCompressionRestorationDescriptorSha256.empty() || job.compressionRestorationStrength != 0.0F || job.compressionRestorationTiled)
            throw std::invalid_argument("A compression restoration model, descriptor hash and strength must be supplied together");
        return;
    }
    if (job.compressionRestorationStrength <= 0.0F)
        throw std::invalid_argument("Compression restoration strength must be greater than zero when a model is selected");
    const auto model = restoration::loadModelPack(job.compressionRestorationModelDirectory);
    if (!restoration::supportsStdfCudaModel(model))
        throw std::invalid_argument("The selected compression restoration model is not the strictly qualified STDF CUDA model");
    const auto descriptorHash = inference::sha256File(model.directory / "model.json",256 * 1024);
    if (!job.expectedCompressionRestorationDescriptorSha256.empty() &&
        job.expectedCompressionRestorationDescriptorSha256 != descriptorHash)
        throw std::invalid_argument("Selected compression restoration model differs from its pinned local descriptor");
    job.compressionRestorationModelDirectory = model.directory;
    job.expectedCompressionRestorationDescriptorSha256 = descriptorHash;
}
std::string serializeIdentity(const ProcessingIdentity& i) {
    if (!adapterIdentifier(i.adapterId) || i.adapterVersion <= 0) throw std::invalid_argument("Processing identity has an invalid model adapter");
    return Json{{"schemaVersion",2},{"source",{{"size",i.source.size},{"modified",i.source.modified},{"fileId",i.source.fileId},{"sha256",i.source.sha256}}},
        {"model",{{"id",i.modelId},{"version",i.modelVersion},{"sha256",i.modelSha256},{"inferenceBackend",i.backend},
            {"adapter",{{"id",i.adapterId},{"version",i.adapterVersion}}}}},
        {"device",{{"ordinal",i.device.ordinal},{"name",i.device.name},{"pciBusId",i.device.pciBusId},{"computeMajor",i.device.computeMajor},{"computeMinor",i.device.computeMinor},{"driverCudaApiVersion",i.device.driverCudaApiVersion}}}}.dump();
}
void applyStoredIdentity(ProcessingJob& job, const std::string& value) {
    if (value.empty()) return;
    const auto json = Json::parse(value);
    const auto [adapterId,adapterVersion] = identityAdapter(json);
    const auto source = json.at("source").at("sha256").get<std::string>(); hash(source,true);
    const auto& modelIdentity = json.at("model");
    const auto model = modelIdentity.at("sha256").get<std::string>(); hash(model,true);
    const auto modelId = modelIdentity.at("id").get<std::string>();
    const auto modelVersion = modelIdentity.at("version").get<std::string>();
    if ((!job.expectedSourceSha256.empty() && job.expectedSourceSha256 != source) ||
        (!job.expectedModelSha256.empty() && job.expectedModelSha256 != model) ||
        (!job.expectedModelId.empty() && job.expectedModelId != modelId) ||
        (!job.expectedModelVersion.empty() && job.expectedModelVersion != modelVersion) ||
        (!job.expectedModelAdapterId.empty() && (job.expectedModelAdapterId != adapterId || job.expectedModelAdapterVersion != adapterVersion)))
        throw std::invalid_argument("Stored identity contradicts task configuration");
    job.expectedSourceSha256 = source; job.expectedModelSha256 = model;
    job.expectedModelId = modelId; job.expectedModelVersion = modelVersion;
    job.expectedModelAdapterId = adapterId; job.expectedModelAdapterVersion = adapterVersion;
}
std::string serializeProgress(const ProcessingEvent& event) {
    const auto [processedFrames,reusedFrames] = processingCounters(event);
    return Json{{"schemaVersion",1},{"stage",event.stage},{"frames",event.outputFrames},{"aiFrames",event.aiFrames},
        {"processedFrames",processedFrames},{"reusedFrames",reusedFrames},
        {"processingFps",event.processingFps},{"timestamp",optional(event.timestamp)},{"timings",timingBreakdown(event.timings)}}.dump();
}
std::string serializeResult(const PipelineResult& r) {
    const auto [processedFrames,reusedFrames] = processingCounters(r);
    const auto processingFps = processingFrameRate(processedFrames,r.elapsedSeconds);
    return Json{{"schemaVersion",1},{"modelId",r.modelId},{"backend",r.backend},
        {"restorationModelId",r.restorationModelId},{"restorationBackend",r.restorationBackend},
        {"restorationFallbackReason",r.restorationFallbackReason},
        {"restorationScale",{{"numerator",r.restorationScaleNumerator},{"denominator",r.restorationScaleDenominator}}},
        {"temporalRestorationModelId",r.temporalRestorationModelId},{"temporalRestorationBackend",r.temporalRestorationBackend},
        {"temporalRestorationFallbackReason",r.temporalRestorationFallbackReason},
        {"temporalRestorationScale",{{"numerator",r.temporalRestorationScaleNumerator},{"denominator",r.temporalRestorationScaleDenominator}}},
        {"automaticGaussianDenoise",{{"algorithmVersion",r.automaticDenoiseAlgorithmVersion},{"active",r.automaticTemporalDenoiseActive},
            {"analyzedSourceFrames",r.automaticDenoiseAnalyzedFrames},{"confidentSourceFrames",r.automaticDenoiseConfidentFrames},
            {"conditionedSourceFrames",r.automaticDenoiseAppliedFrames},{"countsScope","current_execution"}}},
        {"compressionRestorationModelId",r.compressionRestorationModelId},{"compressionRestorationBackend",r.compressionRestorationBackend},
        {"compressionRestorationFallbackReason",r.compressionRestorationFallbackReason},
        {"automaticCompressionRestoration",{{"algorithmVersion",restoration::automaticCompressionAlgorithmVersion},
            {"analyzer",restoration::automaticCompressionAnalyzerId},{"active",r.automaticCompressionRestorationActive},
            {"analyzedSourceFrames",r.automaticCompressionAnalyzedFrames},{"confidentSourceFrames",r.automaticCompressionConfidentFrames},
            {"conditionedSourceFrames",r.automaticCompressionAppliedFrames},{"countsScope","current_execution_including_replay"}}},
        {"modelAdapter",{{"id",r.adapterId},{"version",r.adapterVersion}}},{"inferenceFallbackReason",r.inferenceFallbackReason},
        {"sourceDecoder",r.sourceDecoder},{"sourceDecodeAcceleration",r.sourceDecodeAcceleration},
        {"sourceDecodeFallbackReason",r.sourceDecodeFallbackReason},
        {"encoder",r.video.encoder},{"sourceFrames",r.sourceFrames},{"outputFrames",r.video.framesSubmitted},
        {"aiFrames",r.aiFrames},{"processedFrames",processedFrames},{"reusedFrames",reusedFrames},
        {"inferenceCalls",r.inferenceCalls},{"originalFrames",r.originalFrames},{"heldFrames",r.heldFrames},{"sceneCuts",r.sceneCuts},{"duplicatePairs",r.duplicatePairs},{"abnormalPairs",r.abnormalPairs},
        {"motionBlur",{{"algorithmVersion",2},{"active",r.motionBlurActive},{"blurredFrames",r.motionBlurredFrames},
            {"fallbackFrames",r.motionBlurFallbackFrames},{"fallbackReasons",r.motionBlurFallbackReasons},
            {"edgeHeldFrames",r.motionBlurEdgeHeldFrames},{"peakWindowFrames",r.motionBlurPeakWindowFrames},
            {"peakPreparedBytes",r.motionBlurPeakPreparedBytes},{"countsScope","current_execution"}}},
        {"recoveredTimestamps",r.recoveredTimestamps},
        {"firstTimestamp",r.video.firstTimestamp.toString()},{"endExclusive",r.video.endExclusive.toString()},{"elapsedSeconds",r.elapsedSeconds},{"processingFps",processingFps},{"encoderFallbackReason",r.encoderFallbackReason},
        {"timings",timingBreakdown(r.timings)},
        {"validation",{{"decodedFrames",r.validation.decodedFrames},{"width",r.validation.width},{"height",r.validation.height},
            {"timeBase",r.validation.timeBase.toString()},{"maximumTimestampError",r.validation.maximumTimestampError.toString()},
            {"durationError",r.validation.durationError.toString()},{"declaredEndError",optional(r.validation.declaredEndError)},{"auxiliaryMaximumEndError",r.validation.auxiliaryMaximumEndError.toString()},{"quantized",r.validation.quantized}}}}.dump();
}
std::string serializeProcessingReport(const ProcessingReportContext& context, const PipelineResult& result) {
    if (context.sourcePath.empty() || context.outputPath.empty() || context.modelVersion.empty() || context.modelSha256.size() != 64)
        throw std::invalid_argument("Processing report is missing source, output or verified model identity");
    if (Rational::parse(context.targetFrameRate) <= Rational(0)) throw std::invalid_argument("Processing report frame rate must be positive");
    if (!std::all_of(context.modelSha256.begin(),context.modelSha256.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Processing report model digest must be lowercase SHA-256");
    if (!std::isfinite(context.endToEndElapsedSeconds) || context.endToEndElapsedSeconds < 0 ||
        !std::isfinite(result.elapsedSeconds) || result.elapsedSeconds < 0)
        throw std::invalid_argument("Processing report contains invalid performance measurements");

    const auto& device = context.device;
    const auto& encoding = context.encoding;
    const auto [processedFrames,reusedFrames] = processingCounters(result);
    const auto endToEndProcessingFps = processingFrameRate(processedFrames,context.endToEndElapsedSeconds);
    const auto pipelineProcessingFps = processingFrameRate(processedFrames,result.elapsedSeconds);
    const auto pipeline = Json::parse(serializeResult(result));
    const Json report{
        {"schemaVersion",1},
        {"application",{{"name","RifeLove"},{"ffmpegRuntime",media::MediaEngine::runtimeVersion()}}},
        {"source",{{"path",context.sourcePath},{"decodedFrames",result.sourceFrames},{"recoveredTimestamps",result.recoveredTimestamps},
            {"decoder",result.sourceDecoder},{"decodeAcceleration",result.sourceDecodeAcceleration},
            {"decodeFallbackReason",result.sourceDecodeFallbackReason},{"endEvidence",result.sourceEndEvidence}}},
        {"output",{{"path",context.outputPath},{"targetFrameRate",context.targetFrameRate},{"encoder",result.video.encoder},
            {"width",result.validation.width},{"height",result.validation.height},
            {"submittedFrames",result.video.framesSubmitted},{"videoPackets",result.video.videoPackets},{"copiedPackets",result.video.copiedPackets},
            {"firstTimestamp",result.video.firstTimestamp.toString()},{"endExclusive",result.video.endExclusive.toString()}}},
        {"model",{{"id",result.modelId},{"version",context.modelVersion},{"sha256",context.modelSha256},{"backend",result.backend},
            {"adapter",{{"id",result.adapterId},{"version",result.adapterVersion}}},
            {"fallbackReason",result.inferenceFallbackReason}}},
        {"restoration",result.restorationModelId.empty() ? Json(nullptr) : Json{{"id",result.restorationModelId},
            {"scale",{{"numerator",result.restorationScaleNumerator},{"denominator",result.restorationScaleDenominator}}},
            {"backend",result.restorationBackend.empty() ? "unknown" : result.restorationBackend},
            {"fallbackReason",result.restorationFallbackReason}}},
        {"temporalRestoration",result.temporalRestorationModelId.empty() ? Json(nullptr) : Json{{"id",result.temporalRestorationModelId},
            {"scale",{{"numerator",result.temporalRestorationScaleNumerator},{"denominator",result.temporalRestorationScaleDenominator}}},
            {"backend",result.temporalRestorationBackend.empty() ? "unknown" : result.temporalRestorationBackend},
            {"fallbackReason",result.temporalRestorationFallbackReason}}},
        {"gpu",{{"ordinal",device.ordinal},{"name",device.name},{"pciBusId",device.pciBusId},
            {"computeCapability",std::to_string(device.computeMajor) + "." + std::to_string(device.computeMinor)},
            {"driverCudaApiVersion",device.driverCudaApiVersion},{"totalVramBytes",device.totalBytes}}},
        {"encoding",{{"requestedEncoder",encoding.encoder},{"actualEncoder",result.video.encoder},{"pixelFormat",encoding.pixelFormat},
            {"bitRate",encoding.bitRate},{"gopSize",encoding.gopSize},{"maxBFrames",encoding.maxBFrames},
            {"options",encoding.encoderOptions},{"fallbackReason",result.encoderFallbackReason}}},
        {"performance",{{"elapsedSeconds",context.endToEndElapsedSeconds},
            // Keep the established keys for consumers, but define their
            // numerator as newly processed frames. The explicit aliases make
            // the resumed-task semantics discoverable to newer consumers.
            {"endToEndOutputFramesPerSecond",endToEndProcessingFps},
            {"pipelineOutputFramesPerSecond",pipelineProcessingFps},
            {"endToEndProcessedFramesPerSecond",endToEndProcessingFps},
            {"pipelineProcessedFramesPerSecond",pipelineProcessingFps},
            {"outputFrames",result.video.framesSubmitted},
            {"processedFrames",processedFrames},{"reusedFrames",reusedFrames},
            {"aiFrames",result.aiFrames},{"inferenceCalls",result.inferenceCalls},{"originalFrames",result.originalFrames},{"heldFrames",result.heldFrames},
            {"stageTimings",timingBreakdown(result.timings)}}},
        {"qualityDiagnostics",{{"sceneCuts",result.sceneCuts},{"duplicatePairs",result.duplicatePairs},{"abnormalMotionPairs",result.abnormalPairs},
            {"motionBlur",pipeline.at("motionBlur")},
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

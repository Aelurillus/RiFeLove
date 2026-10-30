#include "restoration/model_pack.hpp"

#include "inference/model_pack.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace rifelove::restoration {
namespace {
using Json = nlohmann::json;

bool isHash(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool safeToken(const std::string& value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

std::string requireString(const Json& object, const char* key, std::size_t maximum = 256) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string())
        throw std::invalid_argument(std::string("Missing or invalid restoration model metadata: ") + key);
    auto value = found->get<std::string>();
    if (value.empty() || value.size() > maximum)
        throw std::invalid_argument(std::string("Missing or oversized restoration model metadata: ") + key);
    return value;
}

const Json& requireObject(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_object())
        throw std::invalid_argument(std::string("Missing or invalid restoration model object: ") + key);
    return *found;
}

int requireInteger(const Json& object, const char* key, int minimum, int maximum) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer())
        throw std::invalid_argument(std::string("Missing or invalid restoration model integer: ") + key);
    std::int64_t value = 0;
    if (found->is_number_unsigned()) {
        const auto unsignedValue = found->get<std::uint64_t>();
        if (unsignedValue > static_cast<std::uint64_t>(maximum))
            throw std::invalid_argument(std::string("Restoration model integer is out of range: ") + key);
        value = static_cast<std::int64_t>(unsignedValue);
    } else {
        value = found->get<std::int64_t>();
    }
    if (value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Restoration model integer is out of range: ") + key);
    return static_cast<int>(value);
}

bool requireBoolean(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean())
        throw std::invalid_argument(std::string("Missing or invalid restoration model flag: ") + key);
    return found->get<bool>();
}

std::vector<std::string> requireStringArray(const Json& object, const char* key, std::size_t maximumCount = 64) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array() || found->empty() || found->size() > maximumCount)
        throw std::invalid_argument(std::string("Missing or invalid restoration model list: ") + key);
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto& item : *found) {
        if (!item.is_string())
            throw std::invalid_argument(std::string("Invalid restoration model list item: ") + key);
        auto value = item.get<std::string>();
        if (!safeToken(value) || std::find(values.begin(), values.end(), value) != values.end())
            throw std::invalid_argument(std::string("Invalid or duplicate restoration model list item: ") + key);
        values.push_back(std::move(value));
    }
    return values;
}

Json parseJson(const std::vector<std::byte>& bytes, const char* label) {
    if (bytes.empty()) throw std::invalid_argument(std::string("Empty restoration model ") + label);
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    auto value = Json::parse(begin, begin + bytes.size());
    if (!value.is_object()) throw std::invalid_argument(std::string("Restoration model ") + label + " must be a JSON object");
    return value;
}

std::vector<std::byte> readVerified(const std::filesystem::path& directory, const Json& metadata,
    const char* fileKey, const char* hashKey, std::size_t limit) {
    const auto relative = requireString(metadata, fileKey);
    const auto expected = requireString(metadata, hashKey, 64);
    if (!isHash(expected)) throw std::invalid_argument(std::string("Invalid restoration SHA-256: ") + hashKey);
    return inference::readVerifiedFile(inference::packFile(directory, relative), expected, limit);
}

Task parseTask(const std::string& value) {
    if (value == "denoise") return Task::denoise;
    if (value == "deblur") return Task::deblur;
    if (value == "deblock") return Task::deblock;
    if (value == "super-resolution") return Task::superResolution;
    throw std::invalid_argument("Unsupported restoration task: " + value);
}

Placement parsePlacement(const std::string& value) {
    if (value == "source-rate-before-interpolation") return Placement::sourceRateBeforeInterpolation;
    if (value == "output-rate-after-interpolation") return Placement::outputRateAfterInterpolation;
    throw std::invalid_argument("Unsupported restoration pipeline placement: " + value);
}

std::string taskName(Task value) {
    switch (value) {
        case Task::denoise: return "denoise";
        case Task::deblur: return "deblur";
        case Task::deblock: return "deblock";
        case Task::superResolution: return "super-resolution";
    }
    throw std::invalid_argument("Invalid restoration task");
}

std::string placementName(Placement value) {
    switch (value) {
        case Placement::sourceRateBeforeInterpolation: return "source-rate-before-interpolation";
        case Placement::outputRateAfterInterpolation: return "output-rate-after-interpolation";
    }
    throw std::invalid_argument("Invalid restoration pipeline placement");
}

Json capabilityEvidence(const Capabilities& capabilities) {
    Json tasks = Json::array();
    for (const auto task : capabilities.tasks) tasks.push_back(taskName(task));
    Json placements = Json::array();
    for (const auto placement : capabilities.placements) placements.push_back(placementName(placement));
    return Json{
        {"tasks", tasks}, {"placements", placements},
        {"supportedInputProfiles", capabilities.supportedInputProfiles},
        {"temporalFramesBefore", capabilities.temporalFramesBefore},
        {"temporalFramesAfter", capabilities.temporalFramesAfter},
        {"scaleNumerator", capabilities.scaleNumerator},
        {"scaleDenominator", capabilities.scaleDenominator},
        {"supportsBoundaryPadding", capabilities.supportsBoundaryPadding}
    };
}

Capabilities parseCapabilities(const Json& metadata, ModelPack& model) {
    Capabilities capabilities;
    capabilities.modelId = model.id;
    capabilities.adapterId = model.capabilities.adapterId;
    capabilities.adapterVersion = model.capabilities.adapterVersion;
    for (const auto& task : requireStringArray(metadata, "tasks")) capabilities.tasks.push_back(parseTask(task));
    for (const auto& placement : requireStringArray(metadata, "placements")) capabilities.placements.push_back(parsePlacement(placement));
    capabilities.supportedInputProfiles = requireStringArray(metadata, "supportedInputProfiles");
    const std::vector<std::string> knownProfiles{"bt709-sdr8", "bt470bg-sdr8", "bt709-sdr10", "bt470bg-sdr10"};
    for (const auto& profile : capabilities.supportedInputProfiles)
        if (std::find(knownProfiles.begin(), knownProfiles.end(), profile) == knownProfiles.end())
            throw std::invalid_argument("Unsupported restoration input profile: " + profile);
    capabilities.temporalFramesBefore = static_cast<std::size_t>(requireInteger(metadata, "temporalFramesBefore", 0, 16));
    capabilities.temporalFramesAfter = static_cast<std::size_t>(requireInteger(metadata, "temporalFramesAfter", 0, 16));
    capabilities.scaleNumerator = requireInteger(metadata, "scaleNumerator", 1, 8);
    capabilities.scaleDenominator = requireInteger(metadata, "scaleDenominator", 1, 8);
    capabilities.supportsBoundaryPadding = requireBoolean(metadata, "supportsBoundaryPadding");
    const auto ratioNumerator = static_cast<std::int64_t>(capabilities.scaleNumerator);
    const auto ratioDenominator = static_cast<std::int64_t>(capabilities.scaleDenominator);
    if (ratioNumerator > 8 * ratioDenominator || ratioDenominator > 8 * ratioNumerator)
        throw std::invalid_argument("Restoration model scale ratio exceeds 8x");
    const bool upscale = std::find(capabilities.tasks.begin(), capabilities.tasks.end(), Task::superResolution) != capabilities.tasks.end();
    if (upscale ? ratioNumerator <= ratioDenominator : ratioNumerator != ratioDenominator)
        throw std::invalid_argument("Restoration scale must match the declared task");
    return capabilities;
}

void validateReport(const Json& report, ModelPack& model) {
    const auto status = requireString(report, "status", 128);
    if (status.rfind("passed_", 0) != 0 || requireString(report, "model_sha256", 64) != model.graphSha256 ||
        requireString(report, "numerical_profile", 128) != model.numericalProfile ||
        requireStringArray(report, "validated_backends") != model.validatedBackends)
        throw std::invalid_argument("Restoration model validation report does not match its declared profile");

    const auto& adapter = requireObject(report, "model_adapter");
    if (requireString(adapter, "id", 128) != model.capabilities.adapterId ||
        requireInteger(adapter, "version", 1, std::numeric_limits<int>::max()) != model.capabilities.adapterVersion)
        throw std::invalid_argument("Restoration model validation report does not match its adapter");
    const auto& evidence = requireObject(report, "capabilities");
    if (evidence != capabilityEvidence(model.capabilities))
        throw std::invalid_argument("Restoration model validation report does not match its capabilities");
    const auto cases = report.find("cases");
    if (cases == report.end() || !cases->is_array() || cases->empty())
        throw std::invalid_argument("Restoration model validation report contains no cases");
    for (const auto& item : *cases)
        if (!item.is_object() || !item.contains("passed") || item.at("passed") != true)
            throw std::invalid_argument("Restoration model validation report contains a failed or malformed case");
}
}

bool supportsOnnxCudaOutputRateAdapter(const Capabilities& capabilities) {
    const auto task = outputRateImageTask(capabilities);
    return capabilities.adapterVersion == 1 &&
        ((task == Task::superResolution && capabilities.adapterId == "image-sr-rgb-f32-nchw-v1") ||
         (task == Task::deblur && capabilities.adapterId == "nafnet-zoo-deblur-rgb-f32-nchw-v1"));
}

bool supportsOnnxCudaOutputRateModel(const ModelPack& model) {
    if (!model.metadataIntegrityVerified || !supportsOnnxCudaOutputRateAdapter(model.capabilities)) return false;
    if (outputRateImageTask(model.capabilities) != Task::deblur) return true;
    return model.numericalProfile == "sdr8-fp32-cpu-reference" &&
        std::find(model.validatedBackends.begin(),model.validatedBackends.end(),"onnxruntime_cpu_source") != model.validatedBackends.end() &&
        std::find(model.validatedBackends.begin(),model.validatedBackends.end(),"onnxruntime_cpu_derived") != model.validatedBackends.end();
}

bool supportsOnnxCudaTemporalAdapter(const Capabilities& caps) {
    return caps.adapterId == "fastdvdnet-rgb-f32-nchw16-v1" && caps.adapterVersion == 1 &&
        caps.tasks == std::vector<Task>{Task::denoise} &&
        caps.placements == std::vector<Placement>{Placement::sourceRateBeforeInterpolation} &&
        caps.temporalFramesBefore == 2 && caps.temporalFramesAfter == 2 && caps.supportsBoundaryPadding &&
        caps.scaleNumerator == 1 && caps.scaleDenominator == 1 &&
        caps.supportedInputProfiles == std::vector<std::string>{"bt709-sdr8"};
}

bool supportsOnnxCudaTemporalModel(const ModelPack& model) {
    return model.metadataIntegrityVerified && supportsOnnxCudaTemporalAdapter(model.capabilities) &&
        model.numericalProfile == "sdr8-fp32-pytorch-reference" &&
        std::find(model.validatedBackends.begin(),model.validatedBackends.end(),"pytorch_cpu_source") != model.validatedBackends.end() &&
        std::find(model.validatedBackends.begin(),model.validatedBackends.end(),"onnxruntime_cpu_derived") != model.validatedBackends.end();
}

ModelPack loadModelPack(const std::filesystem::path& directory) {
    const auto root = std::filesystem::canonical(directory);
    const auto descriptorBytes = inference::readLocalFile(root / "model.json", 256 * 1024);
    const auto descriptor = parseJson(descriptorBytes, "descriptor");
    if (requireInteger(descriptor, "schemaVersion", 1, 1) != 1)
        throw std::invalid_argument("Unsupported restoration model descriptor schema version");

    ModelPack model;
    model.directory = root;
    model.id = requireString(descriptor, "id", 128);
    if (!safeToken(model.id)) throw std::invalid_argument("Unsafe restoration model identifier");
    if (const auto displayName = descriptor.find("displayName"); displayName != descriptor.end()) {
        if (!displayName->is_string()) throw std::invalid_argument("Invalid restoration model display name");
        model.displayName = displayName->get<std::string>();
    } else {
        model.displayName = model.id;
    }
    if (model.displayName.empty() || model.displayName.size() > 256) throw std::invalid_argument("Invalid restoration model display name");
    if (requireString(descriptor, "family", 64) != "video-restoration")
        throw std::invalid_argument("Unsupported model family for the restoration catalog");
    model.version = requireString(descriptor, "version", 128);
    if (!safeToken(model.version)) throw std::invalid_argument("Unsafe restoration model version");

    const auto& adapter = requireObject(descriptor, "adapter");
    model.capabilities.modelId = model.id;
    model.capabilities.adapterId = requireString(adapter, "id", 128);
    if (!safeToken(model.capabilities.adapterId)) throw std::invalid_argument("Unsafe restoration adapter identifier");
    model.capabilities.adapterVersion = requireInteger(adapter, "version", 1, std::numeric_limits<int>::max());

    const auto graphFile = requireString(descriptor, "modelFile", 512);
    model.graphPath = inference::packFile(root, graphFile);
    auto extension = model.graphPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".onnx") throw std::invalid_argument("Restoration model graph must be an ONNX file");
    model.graphSha256 = requireString(descriptor, "sha256", 64);
    if (!isHash(model.graphSha256)) throw std::invalid_argument("Invalid restoration graph SHA-256");
    if (inference::sha256File(model.graphPath, 2ULL * 1024 * 1024 * 1024) != model.graphSha256)
        throw std::runtime_error("Restoration model graph SHA-256 mismatch");

    const auto& license = requireObject(descriptor, "license");
    model.licenseId = requireString(license, "id", 128);
    const auto licenseBytes = readVerified(root, license, "file", "sha256", 1024 * 1024);
    if (licenseBytes.empty()) throw std::invalid_argument("Restoration model license file is empty");
    model.licenseSha256 = requireString(license, "sha256", 64);

    const auto& provenance = requireObject(descriptor, "provenance");
    const auto sourceBytes = readVerified(root, provenance, "manifestFile", "manifestSha256", 256 * 1024);
    model.sourceManifestSha256 = requireString(provenance, "manifestSha256", 64);
    const auto source = parseJson(sourceBytes, "source manifest");
    if (requireString(source, "license", 128) != model.licenseId)
        throw std::invalid_argument("Restoration model license does not match its source manifest");
    model.sourceRepository = requireString(source, "upstreamRepository", 2048);
    model.upstreamCommit = requireString(source, "upstreamCommit", 128);
    if (const auto evidence = source.find("licenseScopeEvidence"); evidence != source.end() && evidence->is_string())
        model.licenseScopeEvidence = evidence->get<std::string>();

    const auto& capabilities = requireObject(descriptor, "capabilities");
    model.capabilities = parseCapabilities(capabilities, model);
    const auto& validation = requireObject(descriptor, "validation");
    model.numericalProfile = requireString(validation, "numericalProfile", 128);
    model.validatedBackends = requireStringArray(validation, "validatedBackends");
    const auto reportBytes = readVerified(root, validation, "reportFile", "reportSha256", 1024 * 1024);
    model.validationSha256 = requireString(validation, "reportSha256", 64);
    const auto report = parseJson(reportBytes, "validation report");
    validateReport(report, model);

    model.metadataIntegrityVerified = true;
    return model;
}

} // namespace rifelove::restoration

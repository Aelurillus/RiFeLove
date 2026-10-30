#include "inference/model_pack.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace rifelove::inference {
namespace {
using Json = nlohmann::json;

bool isHash(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string requireString(const Json& object, const char* key, std::size_t maximum = 256) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) throw std::invalid_argument(std::string("Missing or invalid model metadata: ") + key);
    auto value = found->get<std::string>();
    if (value.empty() || value.size() > maximum) throw std::invalid_argument(std::string("Missing or oversized model metadata: ") + key);
    return value;
}

std::vector<std::string> requireStringArray(const Json& object, const char* key, std::size_t maximumCount = 64) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array() || found->empty() || found->size() > maximumCount)
        throw std::invalid_argument(std::string("Missing or invalid model metadata list: ") + key);
    std::vector<std::string> values;
    values.reserve(found->size());
    for (const auto& item : *found) {
        if (!item.is_string()) throw std::invalid_argument(std::string("Invalid model metadata list item: ") + key);
        auto value = item.get<std::string>();
        if (value.empty() || value.size() > 128 || std::find(values.begin(), values.end(), value) != values.end())
            throw std::invalid_argument(std::string("Invalid or duplicate model metadata list item: ") + key);
        values.push_back(std::move(value));
    }
    return values;
}

Json parseJson(std::span<const std::byte> bytes, const char* label) {
    if (bytes.empty()) throw std::invalid_argument(std::string("Empty model ") + label);
    auto value = Json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    if (!value.is_object()) throw std::invalid_argument(std::string("Model ") + label + " must be a JSON object");
    return value;
}

std::vector<std::byte> readVerifiedPackFile(const std::filesystem::path& directory, const Json& metadata,
    const char* fileKey, const char* hashKey, std::size_t limit) {
    const auto relative = requireString(metadata, fileKey);
    const auto expected = requireString(metadata, hashKey, 64);
    if (!isHash(expected)) throw std::invalid_argument(std::string("Invalid SHA-256 in model metadata: ") + hashKey);
    return readVerifiedFile(packFile(directory, relative), expected, limit);
}

void validateTensorContract(const Json& capabilities) {
    if (!capabilities.is_object() || capabilities.at("inputLayout") != "NCHW" ||
        capabilities.at("dtype") != "float32" || capabilities.at("batch") != 1 || capabilities.at("channels") != 3 ||
        capabilities.at("inputRange") != Json::array({0.0, 1.0}) ||
        capabilities.at("timestepShape") != Json::array({1,1,1,1}) ||
        capabilities.at("dynamicTimestep") != true || capabilities.at("dynamicSpatial") != true)
        throw std::invalid_argument("Unsupported RIFE model tensor contract");
}

void readAdapterMetadata(const Json& descriptor, ModelPack& model) {
    const auto found = descriptor.find("adapter");
    if (found == descriptor.end()) return; // Compatibility with existing RIFE packs.
    if (!found->is_object()) throw std::invalid_argument("Model adapter metadata must be an object");
    model.adapterId = requireString(*found,"id",128);
    const auto version = found->find("version");
    if (version == found->end() || !version->is_number_integer())
        throw std::invalid_argument("Model adapter version is missing or invalid");
    if (version->is_number_unsigned()) {
        const auto value = version->get<std::uint64_t>();
        if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Model adapter version is out of range");
        model.adapterVersion = static_cast<int>(value);
    } else {
        const auto value = version->get<std::int64_t>();
        if (value <= 0 || value > std::numeric_limits<int>::max())
            throw std::invalid_argument("Model adapter version is out of range");
        model.adapterVersion = static_cast<int>(value);
    }
    if (!std::all_of(model.adapterId.begin(),model.adapterId.end(),[](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        }))
        throw std::invalid_argument("Model adapter identifier contains unsupported characters");
}

std::vector<std::byte> readLegacyLicense(const std::filesystem::path& directory, const Json& descriptor, std::string& licenseId) {
    licenseId = requireString(descriptor, "license", 128);
    return readLocalFile(packFile(directory, requireString(descriptor, "licenseFile")), 1024 * 1024);
}

void readV2Metadata(const std::filesystem::path& directory, const Json& descriptor, ModelPack& model) {
    const auto& license = descriptor.at("license");
    if (!license.is_object()) throw std::invalid_argument("Model license metadata must be an object");
    model.licenseId = requireString(license, "id", 128);
    const auto licenseBytes = readVerifiedPackFile(directory, license, "file", "sha256", 1024 * 1024);
    if (licenseBytes.empty()) throw std::invalid_argument("Model license file is empty");
    model.licenseSha256 = requireString(license, "sha256", 64);

    const auto& provenance = descriptor.at("provenance");
    if (!provenance.is_object()) throw std::invalid_argument("Model provenance metadata must be an object");
    const auto sourceBytes = readVerifiedPackFile(directory, provenance, "manifestFile", "manifestSha256", 256 * 1024);
    model.sourceManifestSha256 = requireString(provenance, "manifestSha256", 64);
    const auto source = parseJson(sourceBytes, "source manifest");
    if (requireString(source, "license", 128) != model.licenseId)
        throw std::invalid_argument("Model license does not match its source manifest declaration");
    model.sourceRepository = requireString(source, "upstreamRepository", 2048);
    model.upstreamCommit = requireString(source, "upstreamCommit", 128);
    if (const auto evidence = source.find("licenseScopeEvidence"); evidence != source.end() && evidence->is_string())
        model.licenseScopeEvidence = evidence->get<std::string>();

    const auto& validation = descriptor.at("validation");
    if (!validation.is_object()) throw std::invalid_argument("Model validation metadata must be an object");
    model.numericalProfile = requireString(validation, "numericalProfile", 128);
    model.validatedBackends = requireStringArray(validation, "validatedBackends");
    const auto reportBytes = readVerifiedPackFile(directory, validation, "reportFile", "reportSha256", 1024 * 1024);
    model.validationSha256 = requireString(validation, "reportSha256", 64);
    const auto report = parseJson(reportBytes, "validation report");
    const auto status = requireString(report, "status", 128);
    if (status.rfind("passed_", 0) != 0 || requireString(report, "model_sha256", 64) != model.sha256 ||
        requireString(report, "numerical_profile", 128) != model.numericalProfile ||
        requireStringArray(report, "validated_backends") != model.validatedBackends)
        throw std::invalid_argument("Model validation report does not match the declared model profile");
    const auto reportAdapter = report.find("model_adapter");
    if (reportAdapter == report.end()) {
        if (model.adapterId != "rife-sdr-v1" || model.adapterVersion != 1)
            throw std::invalid_argument("Custom model adapter is missing from its numerical validation report");
    } else {
        if (!reportAdapter->is_object() || !reportAdapter->contains("id") || !reportAdapter->at("id").is_string() ||
            reportAdapter->at("id").get<std::string>() != model.adapterId || !reportAdapter->contains("version") ||
            !reportAdapter->at("version").is_number_integer())
            throw std::invalid_argument("Model validation report does not match the selected adapter");
        const auto version = reportAdapter->at("version");
        int reportVersion = -1;
        if (version.is_number_unsigned()) {
            const auto value = version.get<std::uint64_t>();
            if (value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) reportVersion = static_cast<int>(value);
        } else {
            const auto value = version.get<std::int64_t>();
            if (value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max()) reportVersion = static_cast<int>(value);
        }
        if (reportVersion != model.adapterVersion)
            throw std::invalid_argument("Model validation report does not match the selected adapter");
    }
    const auto cases = report.find("cases");
    if (cases == report.end() || !cases->is_array() || cases->empty())
        throw std::invalid_argument("Model validation report contains no cases");
    for (const auto& item : *cases) {
        if (!item.is_object() || !item.contains("passed") || item.at("passed") != true)
            throw std::invalid_argument("Model validation report contains a failed or malformed case");
    }

    const auto& capabilities = descriptor.at("capabilities");
    validateTensorContract(capabilities);
    model.spatialMultiple = capabilities.at("spatialMultiple").get<int>();
    if (model.spatialMultiple != 64) throw std::invalid_argument("Unsupported RIFE spatial multiple");
    model.supportedInputProfiles = requireStringArray(capabilities, "supportedInputProfiles");
    const std::vector<std::string> knownProfiles{"bt709-sdr8","bt470bg-sdr8","bt709-sdr10","bt470bg-sdr10","hdr","legacy-unverified"};
    for (const auto& profile : model.supportedInputProfiles)
        if (std::find(knownProfiles.begin(),knownProfiles.end(),profile) == knownProfiles.end())
            throw std::invalid_argument("Unknown RIFE input profile: " + profile);
    if (!capabilities.contains("hdrQualified") || !capabilities.at("hdrQualified").is_boolean())
        throw std::invalid_argument("Model HDR qualification metadata is missing");
    model.hdrQualified = capabilities.at("hdrQualified").get<bool>();
    if (!model.hdrQualified && std::find(model.supportedInputProfiles.begin(), model.supportedInputProfiles.end(), "hdr") != model.supportedInputProfiles.end())
        throw std::invalid_argument("Model cannot claim an HDR profile without HDR qualification");
    if (const auto declaredProfiles = report.find("supported_input_profiles"); declaredProfiles != report.end() &&
        (!declaredProfiles->is_array() || requireStringArray(report,"supported_input_profiles") != model.supportedInputProfiles))
        throw std::invalid_argument("Model validation report does not match the declared input profiles");
    model.metadataIntegrityVerified = true;
}

void cryptoCheck(NTSTATUS status) { if (status < 0) throw std::runtime_error("Windows SHA-256 operation failed"); }
}

std::string sha256(std::span<const std::byte> bytes) {
    struct Algorithm { BCRYPT_ALG_HANDLE handle{}; ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); } } algorithm;
    cryptoCheck(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    struct Hash { BCRYPT_HASH_HANDLE handle{}; ~Hash() { if (handle) BCryptDestroyHash(handle); } } hash;
    cryptoCheck(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0));
    while (!bytes.empty()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        cryptoCheck(BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), count, 0));
        bytes = bytes.subspan(count);
    }
    std::array<unsigned char, 32> digest{};
    cryptoCheck(BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : digest) { output += digits[byte >> 4]; output += digits[byte & 15]; }
    return output;
}

std::string sha256File(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto encodedPath = path.u8string();
    const std::string displayPath(encodedPath.begin(),encodedPath.end());
    if (!stream || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Could not open local model file: " + displayPath);
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > limit)
        throw std::runtime_error("Model file exceeds its size limit: " + displayPath);

    struct Algorithm { BCRYPT_ALG_HANDLE handle{}; ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); } } algorithm;
    cryptoCheck(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    struct Hash { BCRYPT_HASH_HANDLE handle{}; ~Hash() { if (handle) BCryptDestroyHash(handle); } } hash;
    cryptoCheck(BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0));

    stream.seekg(0);
    std::array<std::byte, 1024 * 1024> buffer{};
    std::uintmax_t totalRead = 0;
    while (stream) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0) {
            const auto bytesRead = static_cast<std::uintmax_t>(count);
            if (bytesRead > static_cast<std::uintmax_t>(limit) - totalRead)
                throw std::runtime_error("Model file exceeds its size limit: " + displayPath);
            totalRead += bytesRead;
            cryptoCheck(BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0));
        }
    }
    if (!stream.eof()) throw std::runtime_error("Could not read complete local model file: " + displayPath);
    if (totalRead != static_cast<std::uintmax_t>(end))
        throw std::runtime_error("Local model file changed while being verified: " + displayPath);

    std::array<unsigned char, 32> digest{};
    cryptoCheck(BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto byte : digest) { output += digits[byte >> 4]; output += digits[byte & 15]; }
    return output;
}

bool supportsInputProfile(const ModelPack& model, std::string_view profile) {
    if (std::find(model.supportedInputProfiles.begin(),model.supportedInputProfiles.end(),profile) != model.supportedInputProfiles.end()) return true;
    const bool isSdr8 = profile.ends_with("-sdr8");
    return isSdr8 && std::find(model.supportedInputProfiles.begin(),model.supportedInputProfiles.end(),"legacy-unverified") != model.supportedInputProfiles.end();
}

std::vector<std::byte> readLocalFile(const std::filesystem::path& path, std::size_t limit) {
    // Bound allocation, then read from the same opened handle that supplied size.
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto encodedPath = path.u8string();
    const std::string displayPath(encodedPath.begin(),encodedPath.end());
    if (!stream || !std::filesystem::is_regular_file(path))
        throw std::runtime_error("Could not open local model file: " + displayPath);
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > limit)
        throw std::runtime_error("Model file exceeds its size limit: " + displayPath);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read complete local model file: " + displayPath);
    return bytes;
}
std::vector<std::byte> readVerifiedFile(const std::filesystem::path& path, const std::string& expected, std::size_t limit) {
    if (!isHash(expected)) throw std::invalid_argument("Expected a lowercase SHA-256 digest");
    auto bytes = readLocalFile(path, limit);
    if (sha256(bytes) != expected) throw std::runtime_error("Model file SHA-256 mismatch");
    return bytes;
}
std::filesystem::path packFile(const std::filesystem::path& directory, const std::string& relative) {
    const auto input = std::filesystem::path(std::u8string(relative.begin(), relative.end()));
    if (relative.empty() || input.is_absolute() || input.has_root_name() || relative.find(':') != std::string::npos || relative.find('\0') != std::string::npos)
        throw std::invalid_argument("Model paths must be relative paths inside the pack");
    for (const auto& part : input) if (part == "..") throw std::invalid_argument("Model path escapes its pack");
    const auto root = std::filesystem::canonical(directory);
    const auto path = std::filesystem::canonical(root / input);
    auto base = root.begin(), child = path.begin();
    for (; base != root.end(); ++base, ++child) {
        if (child == path.end() || *base != *child) throw std::invalid_argument("Model path escapes its pack");
    }
    if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("Model entry must be a regular file");
    return path;
}
ModelPack loadModelPack(const std::filesystem::path& directory) {
    const auto bytes = readLocalFile(directory / "model.json", 256 * 1024);
    const auto json = parseJson(bytes, "descriptor");
    ModelPack model;
    model.directory = std::filesystem::canonical(directory);
    model.schemaVersion = json.at("schemaVersion").get<int>();
    if (model.schemaVersion != 1 && model.schemaVersion != 2) throw std::invalid_argument("Unsupported model descriptor schema version");
    model.id = requireString(json, "id", 128);
    model.displayName = json.value("displayName", model.id);
    if (model.displayName.empty() || model.displayName.size() > 256) throw std::invalid_argument("Invalid model display name");
    model.family = requireString(json, "family", 64);
    if (model.family != "rife") throw std::invalid_argument("Unsupported model family");
    readAdapterMetadata(json,model);
    model.version = requireString(json, "version", 128);
    model.sha256 = requireString(json, "sha256", 64);
    if (!isHash(model.sha256)) throw std::invalid_argument("Invalid model graph SHA-256");

    const auto graphPath = packFile(directory, requireString(json, "modelFile"));
    model.graph = readVerifiedFile(graphPath, model.sha256, 512ULL * 1024 * 1024);
    if (model.graph.empty()) throw std::invalid_argument("Empty ONNX model");

    if (model.schemaVersion == 1) {
        validateTensorContract(json);
        model.spatialMultiple = json.at("spatialMultiple").get<int>();
        if (model.spatialMultiple != 64) throw std::invalid_argument("Unsupported RIFE spatial multiple");
        const auto license = readLegacyLicense(directory, json, model.licenseId);
        if (license.empty()) throw std::invalid_argument("Model license file is empty");
        if (const auto found = json.find("numericalProfile"); found != json.end() && found->is_string()) model.numericalProfile = found->get<std::string>();
        if (const auto found = json.find("validatedBackends"); found != json.end() && found->is_array())
            for (const auto& item : *found) if (item.is_string()) model.validatedBackends.push_back(item.get<std::string>());
        if (const auto found = json.find("hdrQualified"); found != json.end() && found->is_boolean()) model.hdrQualified = found->get<bool>();
        model.supportedInputProfiles = {"legacy-unverified"};
    } else {
        readV2Metadata(directory, json, model);
    }
    return model;
}
} // namespace rifelove::inference

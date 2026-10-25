#include "local_profile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rifelove::cli {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxProfileBytes = 64 * 1024;

std::string pathUtf8(const std::filesystem::path& value) {
    const auto encoded = value.u8string();
    return {encoded.begin(), encoded.end()};
}

std::vector<std::byte> readProfileFile(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream || !std::filesystem::is_regular_file(file))
        throw std::runtime_error("Could not open local profile file: " + pathUtf8(file));
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > maxProfileBytes)
        throw std::invalid_argument("Local profile file is too large");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read local profile file: " + pathUtf8(file));
    return bytes;
}

bool validUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7f) { ++index; continue; }
        std::size_t count = 0;
        std::uint32_t value = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 0x07; }
        else return false;
        if (index + count > text.size()) return false;
        for (std::size_t continuation = 1; continuation < count; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index + continuation]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        index += count;
    }
    return true;
}

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::filesystem::path localPathFromUrl(const Json& value, std::string_view field) {
    if (!value.is_string()) throw std::invalid_argument("Profile local path fields must be strings");
    const auto url = value.get<std::string>();
    if (url.empty()) return {};
    constexpr std::string_view prefix = "file:///";
    if (!url.starts_with(prefix) || url.find_first_of("?#") != std::string::npos ||
        !std::all_of(url.begin(), url.end(), [](unsigned char c) { return c >= 0x21 && c < 0x80; }))
        throw std::invalid_argument("Profile paths must use fully encoded local file URLs: " + std::string(field));

    std::string decoded;
    decoded.reserve(url.size() - prefix.size() + 1);
    for (std::size_t index = prefix.size() - 1; index < url.size(); ++index) {
        const char valueByte = url[index];
        if (valueByte == '%') {
            if (index + 2 >= url.size()) throw std::invalid_argument("Profile local file URL has an incomplete escape");
            const auto high = hexDigit(url[index + 1]);
            const auto low = hexDigit(url[index + 2]);
            if (high < 0 || low < 0) throw std::invalid_argument("Profile local file URL has an invalid escape");
            decoded.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        } else {
            decoded.push_back(valueByte);
        }
    }
    if (decoded.empty() || decoded.find('\0') != std::string::npos || !validUtf8(decoded))
        throw std::invalid_argument("Profile local file URL does not contain a valid local path");

#ifdef _WIN32
    if (decoded.size() >= 3 && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
        decoded.erase(decoded.begin());
    if (decoded.starts_with("//") || decoded.starts_with("\\\\"))
        throw std::invalid_argument("Profile paths cannot refer to a network share");
#else
    if (!decoded.starts_with('/'))
        throw std::invalid_argument("Profile paths must be absolute local paths");
#endif
    const std::u8string utf8(decoded.begin(), decoded.end());
    const std::filesystem::path result(utf8);
    if (!result.is_absolute()) throw std::invalid_argument("Profile paths must be absolute local paths");
    return result;
}

int integer(const Json& object, const char* key, int minimum, int maximum) {
    if (!object.at(key).is_number_integer()) throw std::invalid_argument("Profile contains a field with the wrong type");
    const auto value = object.at(key).get<std::int64_t>();
    if (value < minimum || value > maximum) throw std::invalid_argument(std::string("Profile field is out of range: ") + key);
    return static_cast<int>(value);
}

template<std::size_t N>
void requireExactFields(const Json& object, const std::array<std::string_view, N>& fields) {
    if (!object.is_object() || object.size() != fields.size())
        throw std::invalid_argument("Profile file contains unsupported or missing fields");
    for (const auto field : fields)
        if (!object.contains(std::string(field))) throw std::invalid_argument("Profile file is missing a required field");
}

} // namespace

LocalProfile loadLocalProfile(const std::filesystem::path& file) {
    const auto bytes = readProfileFile(file);
    const auto json = Json::parse(reinterpret_cast<const char*>(bytes.data()),
        reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    if (!json.is_object() || json.size() != 3 || !json.contains("format") || !json.contains("version") || !json.contains("profile") ||
        !json.at("format").is_string() || json.at("format") != "rifelove-local-profile" ||
        !json.at("version").is_number_integer() || (json.at("version") != 1 && json.at("version") != 2))
        throw std::invalid_argument("Local profile format or version is not supported");

    const auto& value = json.at("profile");
    constexpr std::array<std::string_view, 11> fields{"name", "rate", "modelUrl", "runtimeUrl", "outputUrl", "encoder", "container", "bitrateKbps", "qualityMode", "quality", "gopSize"};
    const int version = json.at("version").get<int>();
    if (version == 1) requireExactFields(value, fields);
    else {
        constexpr std::array<std::string_view, 12> fieldsV2{"name", "rate", "modelUrl", "runtimeUrl", "outputUrl", "encoder", "container", "bitrateKbps", "qualityMode", "quality", "gopSize", "inferenceBackend"};
        requireExactFields(value, fieldsV2);
    }
    for (const auto key : {"name", "rate", "encoder", "container", "qualityMode"})
        if (!value.at(key).is_string()) throw std::invalid_argument("Profile contains a field with the wrong type");
    if (version >= 2 && !value.at("inferenceBackend").is_string())
        throw std::invalid_argument("Profile contains a field with the wrong type");

    LocalProfile profile;
    profile.name = value.at("name").get<std::string>();
    const auto nameUnits = std::count_if(profile.name.begin(), profile.name.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    if (profile.name.empty() || profile.name.size() > 256 || nameUnits > 64 || profile.name.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw std::invalid_argument("Profile name must contain 1 to 64 characters on one line");
    const auto nameStart = profile.name.find_first_not_of(" \t\v\f");
    if (nameStart == std::string::npos || profile.name.find_last_not_of(" \t\v\f") + 1 != profile.name.size())
        throw std::invalid_argument("Profile name must not be blank or padded with spaces");

    try { profile.targetRate = Rational::parse(value.at("rate").get<std::string>()); }
    catch (const std::exception&) { throw std::invalid_argument("Profile frame rate is invalid"); }
    if (profile.targetRate <= Rational(0)) throw std::invalid_argument("Profile frame rate must be positive");

    const auto encoder = value.at("encoder").get<std::string>();
    profile.container = value.at("container").get<std::string>();
    profile.qualityMode = value.at("qualityMode").get<std::string>();
    profile.inferenceBackend = version >= 2 ? value.at("inferenceBackend").get<std::string>() : "auto";
    if (profile.inferenceBackend != "auto" && profile.inferenceBackend != "cuda" && profile.inferenceBackend != "tensorrt")
        throw std::invalid_argument("Profile inference backend is not supported");
    constexpr std::array<std::string_view, 5> encoders{"auto", "libopenh264", "libkvazaar", "libaom-av1", "h264_nvenc"};
    constexpr std::array<std::string_view, 3> containers{"mkv", "mp4", "mov"};
    if (std::find(encoders.begin(), encoders.end(), encoder) == encoders.end())
        throw std::invalid_argument("Profile encoder is not supported");
    if (std::find(containers.begin(), containers.end(), profile.container) == containers.end())
        throw std::invalid_argument("Profile container is not supported");

    profile.bitrateKbps = integer(value, "bitrateKbps", 250, 200000);
    profile.gopSize = integer(value, "gopSize", 0, 1000);
    profile.quality = integer(value, "quality", 0, profile.qualityMode == "crf" ? 63 : 51);
    profile.encoding.encoder = encoder;
    profile.encoding.gopSize = profile.gopSize;
    if (profile.qualityMode == "bitrate") {
        profile.encoding.bitRate = static_cast<std::int64_t>(profile.bitrateKbps) * 1000;
    } else if (profile.qualityMode == "cq" && encoder == "h264_nvenc") {
        profile.encoding.bitRate = 0;
        profile.encoding.encoderOptions.emplace("rc", "vbr");
        profile.encoding.encoderOptions.emplace("cq", std::to_string(profile.quality));
    } else if (profile.qualityMode == "qp" && encoder == "libkvazaar") {
        profile.encoding.bitRate = 0;
        profile.encoding.encoderOptions.emplace("kvazaar-params", "qp=" + std::to_string(profile.quality));
    } else if (profile.qualityMode == "crf" && encoder == "libaom-av1") {
        profile.encoding.bitRate = 0;
        profile.encoding.encoderOptions.emplace("crf", std::to_string(profile.quality));
    } else {
        throw std::invalid_argument("Selected profile encoder does not support the requested quality mode");
    }

    profile.modelDirectory = localPathFromUrl(value.at("modelUrl"), "modelUrl");
    profile.runtimeDirectory = localPathFromUrl(value.at("runtimeUrl"), "runtimeUrl");
    profile.outputLocation = localPathFromUrl(value.at("outputUrl"), "outputUrl");
    return profile;
}

void applyLocalProfile(const LocalProfile& profile, PipelineSettings& settings,
    std::filesystem::path& modelDirectory, std::filesystem::path& runtimeDirectory) {
    if (profile.modelDirectory.empty() || profile.runtimeDirectory.empty())
        throw std::invalid_argument("Local profile must include model and ONNX Runtime folders");
    settings.targetRate = profile.targetRate;
    settings.inferenceBackend = profile.inferenceBackend;
    settings.encoding = profile.encoding;
    modelDirectory = profile.modelDirectory;
    runtimeDirectory = profile.runtimeDirectory;
}

void overrideEncoder(PipelineSettings& settings, const std::string& encoder) {
    if (settings.encoding.encoder != encoder) {
        settings.encoding.encoderOptions.erase("rc");
        settings.encoding.encoderOptions.erase("cq");
        settings.encoding.encoderOptions.erase("kvazaar-params");
        settings.encoding.encoderOptions.erase("crf");
        if (settings.encoding.bitRate == 0) settings.encoding.bitRate = 8'000'000;
    }
    settings.encoding.encoder = encoder;
}

void overrideBitrate(PipelineSettings& settings, std::int64_t bitRate) {
    settings.encoding.encoderOptions.erase("rc");
    settings.encoding.encoderOptions.erase("cq");
    settings.encoding.encoderOptions.erase("kvazaar-params");
    settings.encoding.encoderOptions.erase("crf");
    settings.encoding.bitRate = bitRate;
}

void validateLocalProfileOutput(const LocalProfile& profile, const std::filesystem::path& output) {
    auto extension = output.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto expected = "." + profile.container;
    if (extension != expected)
        throw std::invalid_argument("Output file extension must match the local profile container (." + profile.container + ")");
}

std::string serializeLocalProfileSummary(const LocalProfile& profile) {
    Json output = {{"event", "profile_valid"}, {"name", profile.name}, {"version", 2},
        {"rate", profile.targetRate.toString()}, {"model_directory", pathUtf8(profile.modelDirectory)},
        {"runtime_directory", pathUtf8(profile.runtimeDirectory)}, {"output_location", pathUtf8(profile.outputLocation)},
        {"encoder", profile.encoding.encoder}, {"container", profile.container}, {"quality_mode", profile.qualityMode},
        {"inference_backend", profile.inferenceBackend},
        {"quality", profile.quality}, {"bitrate_kbps", profile.bitrateKbps}, {"gop_size", profile.gopSize}};
    return output.dump();
}

int runLocalProfileCheckCommand(const std::vector<std::string>& args) {
    if (args.size() != 3) throw std::invalid_argument("profile-check PROFILE_JSON");
    const auto profile = loadLocalProfile(std::filesystem::path(std::u8string(args[2].begin(), args[2].end())));
    std::cout << serializeLocalProfileSummary(profile) << '\n' << std::flush;
    return 0;
}

} // namespace rifelove::cli

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
bool isHash(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
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

std::vector<std::byte> readLocalFile(const std::filesystem::path& path, std::size_t limit) {
    // Bound allocation, then read from the same opened handle that supplied size.
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || !std::filesystem::is_regular_file(path)) throw std::runtime_error("Could not open local model file");
    const auto end = stream.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > limit) throw std::runtime_error("Model file exceeds its size limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read complete local model file");
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
    if (bytes.empty()) throw std::invalid_argument("Empty model descriptor");
    const auto json = nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    if (json.at("schemaVersion") != 1 || json.at("family") != "rife" || json.at("inputLayout") != "NCHW" ||
        json.at("dtype") != "float32" || json.at("batch") != 1 || json.at("channels") != 3 ||
        json.at("inputRange") != nlohmann::json::array({0.0, 1.0}) || json.at("timestepShape") != nlohmann::json::array({1,1,1,1}) ||
        json.at("dynamicTimestep") != true || json.at("dynamicSpatial") != true)
        throw std::invalid_argument("Unsupported RIFE model tensor contract");
    const int multiple = json.at("spatialMultiple").get<int>();
    if (multiple != 64) throw std::invalid_argument("Unsupported RIFE spatial multiple");
    const auto license = readLocalFile(packFile(directory, json.at("licenseFile").get<std::string>()), 1024 * 1024);
    if (license.empty() || json.at("license").get<std::string>().empty()) throw std::invalid_argument("Model license metadata is missing");
    const auto hash = json.at("sha256").get<std::string>();
    auto graph = readVerifiedFile(packFile(directory, json.at("modelFile").get<std::string>()), hash, 512ULL * 1024 * 1024);
    if (graph.empty()) throw std::invalid_argument("Empty ONNX model");
    return {std::filesystem::canonical(directory), json.at("id").get<std::string>(), json.at("version").get<std::string>(), hash, multiple, std::move(graph)};
}
} // namespace rifelove::inference

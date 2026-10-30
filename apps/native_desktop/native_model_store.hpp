#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

namespace rifelove::native_desktop {

// This file contains application preferences only, never model bytes. Every
// registered directory is independently verified before it becomes selectable.
struct NativeModelPreferences {
    std::vector<std::filesystem::path> registered;
    std::vector<std::filesystem::path> excluded;
    // 0: RIFE, 1: output-rate super-resolution, 2: source-rate restoration.
    std::array<std::filesystem::path, 3> defaults{};

    static std::wstring key(const std::filesystem::path& path) {
        auto normalized = std::filesystem::absolute(path).lexically_normal();
        while (normalized.has_relative_path() && normalized.filename().empty()) normalized = normalized.parent_path();
        auto value = normalized.wstring();
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return value;
    }
    static bool contains(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
        const auto wanted = key(path);
        return std::any_of(paths.begin(), paths.end(), [&](const auto& item) { return key(item) == wanted; });
    }
    bool isExcluded(const std::filesystem::path& path) const { return contains(excluded, path); }
    void registerDirectory(const std::filesystem::path& path) {
        if (!contains(registered, path)) registered.push_back(std::filesystem::absolute(path).lexically_normal());
        std::erase_if(excluded, [&](const auto& item) { return key(item) == key(path); });
    }
    void unregisterDirectory(const std::filesystem::path& path) {
        std::erase_if(registered, [&](const auto& item) { return key(item) == key(path); });
        if (!contains(excluded, path)) excluded.push_back(std::filesystem::absolute(path).lexically_normal());
        for (auto& selected : defaults) if (!selected.empty() && key(selected) == key(path)) selected.clear();
    }
};

inline std::string nativeModelUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid local model path encoding");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

inline std::filesystem::path nativeModelPath(const std::string& value) {
    if (value.empty() || value.size() > 131072 || value.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid local model registration path");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid local model registration encoding");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    const std::filesystem::path path(result);
    if (!path.is_absolute() || path.root_name().wstring().size() != 2 || path.root_name().wstring()[1] != L':')
        throw std::runtime_error("Only local drive model directories are allowed");
    return path.lexically_normal();
}

// Refuse network drives and reparse directories before following any model
// path. The model center never opens URLs or fetches remote resources.
inline std::filesystem::path checkedNativeModelDirectory(const std::filesystem::path& requested, bool checkTree = true) {
    const auto path = nativeModelPath(nativeModelUtf8(std::filesystem::absolute(requested).lexically_normal().wstring()));
    const auto root = path.root_path().wstring();
    const UINT drive = GetDriveTypeW(root.c_str());
    if (drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR)
        throw std::runtime_error("Model directory must be on a local drive");
    auto current = path.root_path();
    for (const auto& part : path.relative_path()) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Model directory is missing or uses a reparse point");
    }
    if (!checkTree) return path;
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (++files > 4096) throw std::runtime_error("Model directory contains too many files");
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Model files must not use reparse points");
    }
    return path;
}

inline NativeModelPreferences loadNativeModelPreferences(const std::filesystem::path& file) {
    NativeModelPreferences result;
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        if (error) throw std::runtime_error("Cannot inspect local model preferences");
        return result;
    }
    if (std::filesystem::file_size(file) > 4 * 1024 * 1024) throw std::runtime_error("Local model preferences are too large");
    std::ifstream input(file, std::ios::binary);
    std::string magic;
    std::getline(input, magic);
    if (!magic.empty() && magic.back() == '\r') magic.pop_back();
    if (magic != "RifeLoveLocalModels 1") throw std::runtime_error("Unsupported local model preferences");
    std::string operation, encoded;
    std::size_t records = 0;
    while (input >> operation) {
        if (++records > 1024) throw std::runtime_error("Too many local model registrations");
        int kind = -1;
        if (operation == "default" && !(input >> kind)) throw std::runtime_error("Invalid default model entry");
        if (!(input >> std::quoted(encoded))) throw std::runtime_error("Invalid model registration entry");
        const auto path = nativeModelPath(encoded);
        if (operation == "registered") result.registerDirectory(path);
        else if (operation == "excluded") result.unregisterDirectory(path);
        else if (operation == "default" && kind >= 0 && kind < 3) result.defaults[static_cast<std::size_t>(kind)] = path;
        else throw std::runtime_error("Invalid local model preference operation");
    }
    if (!input.eof()) throw std::runtime_error("Cannot read local model preferences");
    for (auto& path : result.defaults) if (!path.empty() && result.isExcluded(path)) path.clear();
    return result;
}

inline void saveNativeModelPreferences(const std::filesystem::path& file, const NativeModelPreferences& preferences, bool replaceExisting = true) {
    static std::atomic<unsigned long> sequence{0};
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence++);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "RifeLoveLocalModels 1\n";
        for (const auto& path : preferences.registered) output << "registered " << std::quoted(nativeModelUtf8(path.wstring())) << '\n';
        for (const auto& path : preferences.excluded) output << "excluded " << std::quoted(nativeModelUtf8(path.wstring())) << '\n';
        for (std::size_t kind = 0; kind < preferences.defaults.size(); ++kind)
            if (!preferences.defaults[kind].empty()) output << "default " << kind << ' ' << std::quoted(nativeModelUtf8(preferences.defaults[kind].wstring())) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Cannot write local model preferences");
        output.close();
        if (!MoveFileExW(temporary.c_str(), file.c_str(), (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace local model preferences");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace rifelove::native_desktop

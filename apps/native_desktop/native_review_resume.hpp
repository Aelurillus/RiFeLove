#pragma once

#include "native_data_directory.hpp"

#include <bcrypt.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rifelove::native_desktop {

inline std::string nativeReviewSha256(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithmHandle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithmHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize review-session SHA-256");
    struct Algorithm final {
        BCRYPT_ALG_HANDLE handle;
        ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm{algorithmHandle};

    DWORD objectSize = 0, digestSize = 0, returned = 0;
    if (BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) < 0 ||
        BCryptGetProperty(algorithm.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize), &returned, 0) < 0 ||
        objectSize == 0 || digestSize != 32 || value.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
        throw std::runtime_error("Cannot inspect review-session SHA-256 provider");
    std::vector<UCHAR> object(objectSize);
    std::array<UCHAR, 32> digest{};
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    if (BCryptCreateHash(algorithm.handle, &hashHandle, object.data(), objectSize, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create review-session SHA-256 hash");
    struct Hash final {
        BCRYPT_HASH_HANDLE handle;
        ~Hash() { if (handle) BCryptDestroyHash(handle); }
    } hash{hashHandle};
    if (!value.empty() && BCryptHashData(hash.handle,
        reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0) < 0)
        throw std::runtime_error("Cannot hash review-session source report");
    if (BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish review-session SHA-256 hash");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

inline std::filesystem::path nativeReviewResumePath(const std::filesystem::path& dataRoot,
    std::string_view reportFingerprint) {
    if (dataRoot.empty() || reportFingerprint.size() != 64 ||
        !std::all_of(reportFingerprint.begin(), reportFingerprint.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))
        throw std::invalid_argument("Invalid review-session storage root or report fingerprint");
    const std::wstring fileName(reportFingerprint.begin(), reportFingerprint.end());
    return dataRoot / L"review-sessions" / (fileName + L".dpapi");
}

inline bool nativeReviewResumeExists(const std::filesystem::path& dataRoot, std::string_view reportFingerprint) {
    if (dataRoot.empty()) return false;
    const auto path = nativeReviewResumePath(dataRoot, reportFingerprint);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Encrypted review-session entry is not a regular local file");
        (void)checkedNativeModelDirectory(path.parent_path(), false);
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("Cannot inspect encrypted review-session state");
}

inline std::optional<nlohmann::json> loadNativeReviewResume(const std::filesystem::path& dataRoot,
    std::string_view reportFingerprint) {
    constexpr std::size_t maximumPlaintext = 16 * 1024 * 1024;
    constexpr std::size_t maximumCiphertext = maximumPlaintext + 64 * 1024;
    const auto path = nativeReviewResumePath(dataRoot, reportFingerprint);
    (void)checkedNativeModelDirectory(path.parent_path(), false);
    const HANDLE input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw std::runtime_error("Cannot open encrypted review-session state");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (GetFileType(input) != FILE_TYPE_DISK || !GetFileInformationByHandle(input, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(input, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maximumCiphertext) {
        CloseHandle(input);
        throw std::runtime_error("Encrypted review-session state is not a valid bounded local file");
    }
    std::vector<BYTE> ciphertext(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < ciphertext.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(ciphertext.size() - offset, 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(input, ciphertext.data() + offset, requested, &read, nullptr) || read != requested) {
            CloseHandle(input);
            throw std::runtime_error("Cannot read encrypted review-session state");
        }
        offset += read;
    }
    CloseHandle(input);

    DATA_BLOB encrypted{static_cast<DWORD>(ciphertext.size()), ciphertext.data()};
    DATA_BLOB plaintext{};
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plaintext))
        throw std::runtime_error("Windows could not decrypt the saved review session for this user");
    std::string serialized(reinterpret_cast<const char*>(plaintext.pbData), plaintext.cbData);
    SecureZeroMemory(plaintext.pbData, plaintext.cbData);
    LocalFree(plaintext.pbData);
    if (serialized.size() > maximumPlaintext) {
        SecureZeroMemory(serialized.data(), serialized.size());
        throw std::runtime_error("Decrypted review-session state exceeds its size limit");
    }
    try {
        auto state = nlohmann::json::parse(serialized);
        SecureZeroMemory(serialized.data(), serialized.size());
        return state;
    } catch (...) {
        SecureZeroMemory(serialized.data(), serialized.size());
        throw std::runtime_error("Decrypted review-session state is malformed");
    }
}

inline void saveNativeReviewResume(const std::filesystem::path& dataRoot, std::string_view reportFingerprint,
    const nlohmann::json& state) {
    constexpr std::size_t maximumPlaintext = 16 * 1024 * 1024;
    const auto serialized = state.dump();
    if (serialized.empty() || serialized.size() > maximumPlaintext || serialized.size() > std::numeric_limits<DWORD>::max())
        throw std::runtime_error("Review-session state exceeds its size limit");
    DATA_BLOB plaintext{static_cast<DWORD>(serialized.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(serialized.data()))};
    DATA_BLOB ciphertext{};
    if (!CryptProtectData(&plaintext, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &ciphertext))
        throw std::runtime_error("Windows could not encrypt the review-session state");
    struct ProtectedBuffer final {
        BYTE* data;
        DWORD size;
        ~ProtectedBuffer() { if (data) { SecureZeroMemory(data, size); LocalFree(data); } }
    } protectedBuffer{ciphertext.pbData, ciphertext.cbData};

    const auto path = nativeReviewResumePath(dataRoot, reportFingerprint);
    prepareWritableNativeDataDirectory(path.parent_path());
    const DWORD existing = GetFileAttributesW(path.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES && (existing & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Review-session target is not a regular local file");
    if (existing == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Cannot inspect review-session target");
    }

    const auto temporary = uniqueNativeDataPath(path.parent_path(), L".review-session-");
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create encrypted review-session staging file");
    try {
        std::size_t offset = 0;
        while (offset < ciphertext.cbData) {
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(ciphertext.cbData - offset, 1024 * 1024));
            DWORD written = 0;
            if (!WriteFile(output, ciphertext.pbData + offset, requested, &written, nullptr) || written != requested)
                throw std::runtime_error("Cannot write encrypted review-session state");
            offset += written;
        }
        if (!FlushFileBuffers(output)) throw std::runtime_error("Cannot flush encrypted review-session state");
        CloseHandle(output);
        output = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot publish encrypted review-session state");
    } catch (...) {
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        DeleteFileW(temporary.c_str());
        throw;
    }
}

} // namespace rifelove::native_desktop

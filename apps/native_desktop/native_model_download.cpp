#include "native_model_download.hpp"

#include "native_package_archive.hpp"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace rifelove::native_desktop {
namespace {
class InternetHandle final {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
private:
    HINTERNET value_ = nullptr;
};

[[noreturn]] void throwWinHttp(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (Windows error " +
        std::to_string(GetLastError()) + ")");
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Cannot encode the model download URL");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("Cannot encode the model download URL");
    return result;
}

std::wstring headerValue(HINTERNET request, DWORD query) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
            WINHTTP_NO_HEADER_INDEX)) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::uint64_t parseUnsigned(std::wstring_view value, const char* error) {
    std::uint64_t result = 0;
    if (value.empty()) throw std::runtime_error(error);
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') throw std::runtime_error(error);
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) throw std::runtime_error(error);
        result = result * 10 + digit;
    }
    return result;
}

std::string sha256File(const std::filesystem::path& filePath) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize model download SHA-256 verification");
    struct AlgorithmCleanup final { BCRYPT_ALG_HANDLE value; ~AlgorithmCleanup() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithmCleanup{algorithm};
    DWORD objectBytes = 0, hashBytes = 0, received = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes), &received, 0) < 0 ||
        hashBytes != 32) throw std::runtime_error("Cannot prepare model download SHA-256 verification");
    std::vector<UCHAR> object(objectBytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create model download SHA-256 verification");
    struct HashCleanup final { BCRYPT_HASH_HANDLE value; ~HashCleanup() { if (value) BCryptDestroyHash(value); } } hashCleanup{hash};
    std::ifstream input(filePath, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read the completed model download");
    std::array<UCHAR, 1024 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0)
            throw std::runtime_error("Cannot hash the completed model download");
    }
    if (!input.eof()) throw std::runtime_error("Cannot read the complete model download for verification");
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish model download SHA-256 verification");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 0x0f]); }
    return result;
}

void checkRegularFileOrAbsent(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("Cannot inspect the model download cache file");
        return;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        throw std::runtime_error("Model download cache paths must be regular files");
}

std::wstring rangeHeader(std::uint64_t offset) {
    return L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
}

} // namespace

void downloadNativeModelFile(const std::wstring& inputUrl, const std::filesystem::path& destination,
    std::uint64_t expectedSizeBytes, const std::string& expectedSha256, std::uint64_t maximumBytes,
    std::stop_token stopToken, std::function<void(NativeModelDownloadProgress)> progress) {
    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
    if (inputUrl.empty() || inputUrl.size() > 8192 || inputUrl.find_first_of(L"\r\n") != std::wstring::npos ||
        inputUrl.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Invalid model download URL");
    if (maximumBytes == 0 || (expectedSizeBytes != 0 && expectedSizeBytes > maximumBytes))
        throw std::invalid_argument("Invalid model download size limit");

    std::wstring url = inputUrl;
    const auto fragment = url.find(L'#');
    if (fragment != std::wstring::npos) url.resize(fragment);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), ICU_REJECT_USERPWD, &parts))
        throw std::invalid_argument("Model download URL is malformed or contains credentials");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0 || parts.dwUrlPathLength == 0)
        throw std::invalid_argument("Model packages must be downloaded from HTTPS URLs");

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength != 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    const auto partial = std::filesystem::path(destination.native() + L".part");
    const auto metadata = std::filesystem::path(destination.native() + L".part.meta");
    checkRegularFileOrAbsent(destination);
    checkRegularFileOrAbsent(partial);
    checkRegularFileOrAbsent(metadata);
    std::error_code filesystemError;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
        if (filesystemError) throw std::runtime_error("Cannot create the model download cache directory");
    }

    if (expectedSha256.size() != 0 && (expectedSha256.size() != 64 || !std::all_of(expectedSha256.begin(), expectedSha256.end(), [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))) throw std::invalid_argument("Invalid expected model package SHA-256");
    if (expectedSizeBytes != 0 && std::filesystem::exists(destination, filesystemError) && !filesystemError) {
        const auto installedSize = std::filesystem::file_size(destination, filesystemError);
        if (!filesystemError && installedSize == expectedSizeBytes &&
            (expectedSha256.empty() || sha256File(destination) == expectedSha256)) return;
        std::filesystem::remove(destination, filesystemError);
        if (filesystemError) throw std::runtime_error("Cannot replace an incomplete model download cache file");
    }

    const std::string identity = utf8(url) + "\n" + expectedSha256 + "\n" + std::to_string(expectedSizeBytes) + "\n";
    bool resume = false;
    if (expectedSizeBytes != 0 && !expectedSha256.empty() && std::filesystem::exists(partial, filesystemError) && !filesystemError &&
        std::filesystem::exists(metadata, filesystemError) && !filesystemError) {
        const auto metadataSize = std::filesystem::file_size(metadata, filesystemError);
        std::string saved;
        if (!filesystemError && metadataSize <= 16384) {
            std::ifstream savedMetadata(metadata, std::ios::binary);
            saved.assign(std::istreambuf_iterator<char>(savedMetadata), std::istreambuf_iterator<char>());
        }
        const auto partialSize = std::filesystem::file_size(partial, filesystemError);
        resume = !filesystemError && saved == identity && partialSize <= maximumBytes &&
            (expectedSizeBytes == 0 || partialSize <= expectedSizeBytes);
    }
    if (!resume) {
        std::filesystem::remove(partial, filesystemError);
        filesystemError.clear();
        std::filesystem::remove(metadata, filesystemError);
        if (filesystemError) throw std::runtime_error("Cannot reset the model download cache");
        std::ofstream savedMetadata(metadata, std::ios::binary | std::ios::trunc);
        savedMetadata.write(identity.data(), static_cast<std::streamsize>(identity.size()));
        savedMetadata.flush();
        if (!savedMetadata) throw std::runtime_error("Cannot record model download resume identity");
    }

    std::uint64_t offset = 0;
    if (resume) {
        offset = std::filesystem::file_size(partial, filesystemError);
        if (filesystemError) throw std::runtime_error("Cannot inspect a partial model download");
        if (expectedSizeBytes != 0 && offset == expectedSizeBytes) {
            if (!expectedSha256.empty() && sha256File(partial) != expectedSha256) {
                std::filesystem::remove(partial, filesystemError);
                std::filesystem::remove(metadata, filesystemError);
                throw std::runtime_error("Resumed model package failed its SHA-256 check");
            }
            if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot finalize the completed model download");
            std::filesystem::remove(metadata, filesystemError);
            return;
        }
    }

    InternetHandle session(WinHttpOpen(L"RifeLove Model Center/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) throwWinHttp("Open HTTPS session");
    if (!WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 30000)) throwWinHttp("Configure HTTPS timeouts");
    DWORD autoLogonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autoLogonPolicy, sizeof(autoLogonPolicy)))
        throwWinHttp("Disable automatic network credentials");
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection.get()) throwWinHttp("Connect to model host");
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.get()) throwWinHttp("Create model download request");

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)))
        throwWinHttp("Require secure model download redirects");
    if (offset != 0) {
        const auto header = rangeHeader(offset);
        if (!WinHttpAddRequestHeaders(request.get(), header.c_str(), static_cast<DWORD>(header.size()), WINHTTP_ADDREQ_FLAG_ADD))
            throwWinHttp("Request model download resume");
    }
    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) throwWinHttp("Request model package");

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX))
        throwWinHttp("Read model download response");

    if (status == HTTP_STATUS_PARTIAL_CONTENT && offset != 0) {
        const auto contentRange = headerValue(request.get(), WINHTTP_QUERY_CONTENT_RANGE);
        constexpr std::wstring_view prefix = L"bytes ";
        if (!contentRange.starts_with(prefix)) throw std::runtime_error("Model mirror returned an invalid resume range");
        const auto separator = contentRange.find(L'-', prefix.size());
        const auto slash = contentRange.find(L'/', separator == std::wstring::npos ? prefix.size() : separator + 1);
        if (separator == std::wstring::npos || slash == std::wstring::npos ||
            parseUnsigned(std::wstring_view(contentRange).substr(prefix.size(), separator - prefix.size()), "Invalid model resume range") != offset)
            throw std::runtime_error("Model mirror resumed at an unexpected byte offset");
        const auto totalText = std::wstring_view(contentRange).substr(slash + 1);
        if (totalText == L"*" || parseUnsigned(totalText, "Invalid model download length") > maximumBytes)
            throw std::runtime_error("Model mirror returned an unknown or excessive package size");
        const auto totalFromRange = parseUnsigned(totalText, "Invalid model download length");
        if (expectedSizeBytes != 0 && totalFromRange != expectedSizeBytes)
            throw std::runtime_error("Model mirror package size does not match its manifest");
    } else if (status == HTTP_STATUS_OK) {
        offset = 0;
    } else {
        throw std::runtime_error("Model mirror returned HTTP status " + std::to_string(status));
    }

    DWORD contentLength = 0;
    DWORD contentLengthBytes = sizeof(contentLength);
    const bool hasLength = WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthBytes, WINHTTP_NO_HEADER_INDEX) != FALSE;
    const std::uint64_t totalBytes = hasLength ? static_cast<std::uint64_t>(contentLength) + offset : expectedSizeBytes;
    if (totalBytes > maximumBytes || (expectedSizeBytes != 0 && totalBytes != expectedSizeBytes))
        throw std::runtime_error("Model mirror package size does not match the declared limit");

    std::ofstream output(partial, std::ios::binary | (offset == 0 ? std::ios::trunc : std::ios::app));
    if (!output) throw std::runtime_error("Cannot open the model download cache file");
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t completed = offset;
    if (progress) progress({completed, totalBytes});
    for (;;) {
        if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throwWinHttp("Read model package response");
        if (available == 0) break;
        while (available != 0) {
            const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            DWORD received = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), requested, &received)) throwWinHttp("Read model package bytes");
            if (received == 0) break;
            if (received > maximumBytes || completed > maximumBytes - received)
                throw std::runtime_error("Model package exceeds the download size limit");
            output.write(buffer.data(), static_cast<std::streamsize>(received));
            if (!output) throw std::runtime_error("Cannot write the model download cache file");
            completed += received;
            available -= received;
            if (progress) progress({completed, totalBytes});
        }
    }
    output.flush();
    if (!output) throw std::runtime_error("Cannot flush the model download cache file");
    output.close();
    if (expectedSizeBytes != 0 && completed != expectedSizeBytes)
        throw std::runtime_error("Model mirror closed the response before the complete package arrived");
    if (totalBytes != 0 && completed != totalBytes)
        throw std::runtime_error("Model mirror returned a truncated package");
    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
    if (!expectedSha256.empty() && sha256File(partial) != expectedSha256) {
        std::filesystem::remove(partial, filesystemError);
        std::filesystem::remove(metadata, filesystemError);
        throw std::runtime_error("Downloaded model package failed its SHA-256 check");
    }
    if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot finalize the model download cache file");
    std::filesystem::remove(metadata, filesystemError);
}

} // namespace rifelove::native_desktop

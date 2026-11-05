#include "native_package_archive.hpp"

#include "native_model_store.hpp"

#include <miniz.h>

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <map>
#include <set>
#include <share.h>
#include <string_view>
#include <system_error>

namespace rifelove::native_desktop {
namespace {
constexpr std::uint64_t kMaximumArchiveBytes = 1024ULL * 1024 * 1024;
constexpr std::uint64_t kMaximumReadmeBytes = 64ULL * 1024;
constexpr std::size_t kMaximumArchiveEntries = 32;

bool safePackageSegment(std::string_view value) {
    if (value.empty() || value.size() > 128 || !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= '0' && value.front() <= '9'))) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    });
}

bool safeArchiveFileName(std::string_view value) {
    if (value.empty() || value.size() > 128 || !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z') || (value.front() >= '0' && value.front() <= '9'))) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    });
}

bool validSha256(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

void requireCrypto(NTSTATUS status) {
    if (status < 0) throw std::runtime_error("Windows SHA-256 verification failed");
}

struct ZipReader final {
    mz_zip_archive archive{};
    FILE* file = nullptr;
    bool initialized = false;
    ~ZipReader() {
        if (initialized) (void)mz_zip_reader_end(&archive);
        if (file) std::fclose(file);
    }
};

struct ExtractContext final {
    HANDLE output = INVALID_HANDLE_VALUE;
    std::stop_token stopToken;
    std::uint64_t written = 0;
    std::uint64_t maximumBytes = 0;
    NativePackageArchiveProgress* progress = nullptr;
    std::function<void(NativePackageArchiveProgress)>* callback = nullptr;
    bool cancelled = false;
    std::string failure;
};

size_t writeExtractedBytes(void* opaque, mz_uint64 offset, const void* buffer, size_t count) {
    auto& context = *static_cast<ExtractContext*>(opaque);
    if (context.stopToken.stop_requested()) {
        context.cancelled = true;
        return 0;
    }
    if (offset != context.written || count > context.maximumBytes - std::min(context.maximumBytes, context.written)) {
        context.failure = "Archive entry wrote beyond its declared size";
        return 0;
    }
    const auto* source = static_cast<const std::byte*>(buffer);
    std::size_t remaining = count;
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(context.output, source, chunk, &written, nullptr) || written != chunk) {
            context.failure = "Cannot write a verified model package file";
            return 0;
        }
        source += written;
        remaining -= written;
        context.written += written;
        if (context.progress) {
            context.progress->completedBytes += written;
            try {
                if (context.callback && *context.callback) (*context.callback)(*context.progress);
            } catch (...) {
                context.failure = "Cannot update model installation progress";
                return 0;
            }
        }
    }
    return count;
}

std::string zipEntryName(mz_zip_archive& archive, mz_uint index) {
    std::array<char, 1024> name{};
    const mz_uint size = mz_zip_reader_get_filename(&archive, index, name.data(), static_cast<mz_uint>(name.size()));
    if (size < 2 || size >= name.size() || name[size - 1] != '\0')
        throw std::runtime_error("Cannot read a model package entry name");
    const std::string result(name.data(), static_cast<std::size_t>(size - 1));
    if (result.find('\0') != std::string::npos) throw std::runtime_error("Model package entry name contains a null byte");
    return result;
}

FILE* openStableArchive(const std::filesystem::path& archive, std::uint64_t expectedSize) {
    const auto path = std::filesystem::absolute(archive).lexically_normal();
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open the model package archive");
    BY_HANDLE_FILE_INFORMATION information{};
    const bool regular = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &information) &&
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    const std::uint64_t actualSize = regular
        ? (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow
        : 0;
    if (!regular || actualSize != expectedSize) {
        CloseHandle(handle);
        throw std::runtime_error("Model package is not a stable local file of the declared size");
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(handle);
        throw std::runtime_error("Cannot prepare the model package archive reader");
    }
    FILE* file = _fdopen(descriptor, "rb");
    if (!file) {
        _close(descriptor);
        throw std::runtime_error("Cannot prepare the model package archive reader");
    }
    return file;
}

void validateArchiveInput(const std::filesystem::path& archive, std::uint64_t expectedSize) {
    const auto path = std::filesystem::absolute(archive).lexically_normal();
    (void)checkedNativeModelDirectory(path.parent_path(), false);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("Model package must be a regular local file");
    const auto size = std::filesystem::file_size(path);
    if (size == 0 || size > kMaximumArchiveBytes || size != expectedSize)
        throw std::runtime_error("Model package size does not match its manifest or exceeds the archive limit");
}

std::string hashArchive(FILE* file, std::uint64_t expectedSize, std::stop_token stopToken,
    NativePackageArchiveProgress& progress, const std::function<void(NativePackageArchiveProgress)>& callback) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        requireCrypto(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        requireCrypto(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        std::array<unsigned char, 1024 * 1024> buffer{};
        std::uint64_t totalRead = 0;
        progress.phase = "archive";
        progress.completedBytes = 0;
        progress.totalBytes = expectedSize;
        if (callback) callback(progress);
        for (;;) {
            if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
            const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file);
            if (count != 0) {
                if (count > expectedSize - std::min(expectedSize, totalRead))
                    throw std::runtime_error("Model package changed while its archive hash was being checked");
                totalRead += count;
                requireCrypto(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0));
                progress.completedBytes = totalRead;
                if (callback) callback(progress);
            }
            if (count < buffer.size()) {
                if (std::ferror(file)) throw std::runtime_error("Cannot read the complete model package archive");
                break;
            }
        }
        if (totalRead != expectedSize || _fseeki64(file, 0, SEEK_SET) != 0)
            throw std::runtime_error("Model package changed while its archive hash was being checked");
        std::array<unsigned char, 32> digest{};
        requireCrypto(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(digest.size() * 2);
        for (const auto byte : digest) { output += digits[byte >> 4]; output += digits[byte & 15]; }
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        return output;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

std::string hashExtractedFile(const std::filesystem::path& path, std::uint64_t expectedSize,
    std::stop_token stopToken, NativePackageArchiveProgress& progress,
    const std::function<void(NativePackageArchiveProgress)>& callback) {
    HANDLE input = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (input == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reopen a staged model package file for verification");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(input, &information) || (information.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(input);
        throw std::runtime_error("Staged model package file is not a regular file");
    }
    const std::uint64_t actualSize = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    if (actualSize != expectedSize) { CloseHandle(input); throw std::runtime_error("Staged model package file size changed during verification"); }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        requireCrypto(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        requireCrypto(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        std::array<unsigned char, 1024 * 1024> buffer{};
        std::uint64_t totalRead = 0;
        progress.phase = "file";
        progress.completedBytes = 0;
        progress.totalBytes = expectedSize;
        if (callback) callback(progress);
        for (;;) {
            if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
            DWORD count = 0;
            if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                throw std::runtime_error("Cannot read a staged model package file");
            if (count == 0) break;
            if (count > expectedSize - std::min(expectedSize, totalRead))
                throw std::runtime_error("Staged model package file changed during verification");
            totalRead += count;
            requireCrypto(BCryptHashData(hash, buffer.data(), count, 0));
            progress.completedBytes = totalRead;
            if (callback) callback(progress);
        }
        if (totalRead != expectedSize) throw std::runtime_error("Staged model package file changed during verification");
        std::array<unsigned char, 32> digest{};
        requireCrypto(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(digest.size() * 2);
        for (const auto byte : digest) { output += digits[byte >> 4]; output += digits[byte & 15]; }
        BCryptDestroyHash(hash); hash = nullptr;
        BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
        CloseHandle(input);
        return output;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(input);
        throw;
    }
}

} // namespace

std::filesystem::path extractNativeModelPackageArchive(const std::filesystem::path& archive,
    const NativePackageArchivePlan& plan, const std::filesystem::path& stagingDirectory,
    std::stop_token stopToken, std::function<void(NativePackageArchiveProgress)> progress) {
    if (!safePackageSegment(plan.rootDirectory) || plan.archiveSizeBytes == 0 || plan.archiveSizeBytes > kMaximumArchiveBytes ||
        !validSha256(plan.archiveSha256) || plan.files.empty() || plan.files.size() + 1 > kMaximumArchiveEntries ||
        plan.maximumUncompressedBytes == 0 || plan.maximumUncompressedBytes > 512ULL * 1024 * 1024)
        throw std::invalid_argument("Invalid model package extraction plan");

    std::set<std::string> expectedPaths;
    std::uint64_t declaredBytes = kMaximumReadmeBytes;
    for (const auto& item : plan.files) {
        if (!safeArchiveFileName(item.path) || item.path == "." || item.path == ".." || item.sizeBytes == 0 ||
            !validSha256(item.sha256) ||
            !expectedPaths.insert(item.path).second || item.sizeBytes > plan.maximumUncompressedBytes ||
            item.sizeBytes > plan.maximumUncompressedBytes - std::min(plan.maximumUncompressedBytes, declaredBytes))
            throw std::invalid_argument("Invalid model file declaration in the package manifest");
        declaredBytes += item.sizeBytes;
    }
    if (declaredBytes > plan.maximumUncompressedBytes) throw std::invalid_argument("Model package exceeds its uncompressed size limit");
    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();

    validateArchiveInput(archive, plan.archiveSizeBytes);
    ZipReader reader;
    reader.file = openStableArchive(archive, plan.archiveSizeBytes);
    NativePackageArchiveProgress state{};
    if (hashArchive(reader.file, plan.archiveSizeBytes, stopToken, state, progress) != plan.archiveSha256)
        throw std::runtime_error("Model package SHA-256 does not match the selected distribution manifest");
    if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
    if (!mz_zip_reader_init_cfile(&reader.archive, reader.file, plan.archiveSizeBytes, 0))
        throw std::runtime_error("Invalid or unsupported model package ZIP archive");
    reader.initialized = true;

    const auto stage = std::filesystem::absolute(stagingDirectory).lexically_normal();
    (void)checkedNativeModelDirectory(stage.parent_path(), false);
    if (!std::filesystem::create_directory(stage)) throw std::runtime_error("Cannot create a unique model installation staging directory");

    try {
        const mz_uint entryCount = mz_zip_reader_get_num_files(&reader.archive);
        if (entryCount != plan.files.size() + 1 || entryCount > kMaximumArchiveEntries)
            throw std::runtime_error("Model package contains missing or unlisted archive entries");

        std::map<mz_uint, std::filesystem::path> destinations;
        std::set<std::string> seen;
        std::uint64_t totalBytes = 0;
        const auto modelDirectory = stage / std::filesystem::path(plan.rootDirectory);
        if (!std::filesystem::create_directory(modelDirectory)) throw std::runtime_error("Cannot create the model package directory");

        for (mz_uint index = 0; index < entryCount; ++index) {
            if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&reader.archive, index, &stat) || stat.m_is_directory || stat.m_is_encrypted ||
                !stat.m_is_supported || !mz_zip_reader_is_file_supported(&reader.archive, index))
                throw std::runtime_error("Model package contains an unsupported, encrypted, or directory entry");

            const auto name = zipEntryName(reader.archive, index);
            if (name.find('\\') != std::string::npos || name.find('\0') != std::string::npos || !seen.insert(name).second)
                throw std::runtime_error("Model package contains an unsafe or duplicate path");
            if (name == "README.txt") {
                if (stat.m_uncomp_size > kMaximumReadmeBytes) throw std::runtime_error("Model package README exceeds its size limit");
                destinations.emplace(index, stage / L"README.txt");
                totalBytes += stat.m_uncomp_size;
                continue;
            }

            const auto prefix = plan.rootDirectory + "/";
            if (!name.starts_with(prefix)) throw std::runtime_error("Model package contains a path outside its declared model folder");
            const auto relativeName = name.substr(prefix.size());
            if (!safeArchiveFileName(relativeName)) throw std::runtime_error("Model package contains an unsafe model file path");
            const auto expected = std::find_if(plan.files.begin(), plan.files.end(), [&](const auto& item) { return item.path == relativeName; });
            if (expected == plan.files.end() || stat.m_uncomp_size != expected->sizeBytes)
                throw std::runtime_error("Model package file does not match its manifest size and path");
            if (expected->sizeBytes > plan.maximumUncompressedBytes - std::min(plan.maximumUncompressedBytes, totalBytes))
                throw std::runtime_error("Model package exceeds its uncompressed size limit");
            totalBytes += expected->sizeBytes;
            destinations.emplace(index, modelDirectory / std::filesystem::path(relativeName));
        }
        if (!seen.contains("README.txt") || destinations.size() != entryCount || seen.size() != entryCount)
            throw std::runtime_error("Model package is missing its README or a declared file");
        for (const auto& item : plan.files)
            if (!seen.contains(plan.rootDirectory + "/" + item.path))
                throw std::runtime_error("Model package is missing a declared file");

        state.phase = "extract";
        state.completedBytes = 0;
        state.totalBytes = totalBytes;
        if (progress) progress(state);
        for (const auto& [index, destination] : destinations) {
            if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
            HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a staged model package file");
            ExtractContext context{output, stopToken, 0, 0, &state, &progress, false, {}};
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&reader.archive, index, &stat)) {
                CloseHandle(output);
                throw std::runtime_error("Cannot read model package file metadata");
            }
            context.maximumBytes = stat.m_uncomp_size;
            const mz_bool extracted = mz_zip_reader_extract_to_callback(&reader.archive, index, &writeExtractedBytes, &context, 0);
            bool flushed = false;
            if (extracted && context.written == context.maximumBytes) flushed = FlushFileBuffers(output) != FALSE;
            CloseHandle(output);
            if (context.cancelled || stopToken.stop_requested()) throw NativePackageInstallCancelled();
            if (!context.failure.empty()) throw std::runtime_error(context.failure);
            if (!extracted || context.written != context.maximumBytes || !flushed)
                throw std::runtime_error("Model package entry failed CRC, size, or disk-write verification");
        }
        if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
        for (const auto& item : plan.files) {
            if (stopToken.stop_requested()) throw NativePackageInstallCancelled();
            const auto file = modelDirectory / std::filesystem::path(item.path);
            if (hashExtractedFile(file, item.sizeBytes, stopToken, state, progress) != item.sha256)
                throw std::runtime_error("Model package file SHA-256 does not match its distribution manifest");
        }
        return modelDirectory;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(stage, ignored);
        throw;
    }
}

} // namespace rifelove::native_desktop

#include "application/local_lease.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>
#include <vector>

namespace rifelove::application {
namespace {
void checkStop(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("File verification cancelled"); }
void checkHash(NTSTATUS status) { if (status < 0) throw std::runtime_error("Windows SHA-256 operation failed"); }
struct Hash final {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE value = nullptr;
    std::vector<UCHAR> storage;
    ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    void initialize() {
        checkHash(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
        DWORD bytes = 0, length = 0;
        checkHash(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&bytes,0));
        storage.resize(length);
        checkHash(BCryptCreateHash(algorithm,&value,storage.data(),length,nullptr,0,0));
    }
};
}
SourceLease::SourceLease(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not exclusively protect source from writes/replacement (Windows error " + std::to_string(GetLastError()) + ")");
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle,&info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(handle); throw std::invalid_argument("Source lease requires a local regular file, not a directory or reparse point");
    }
    handle_ = handle;
}
SourceLease::~SourceLease() { if (handle_) CloseHandle(handle_); }
FileIdentity SourceLease::metadata() const {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle_,&info)) throw std::runtime_error("Could not read source file identity");
    FileIdentity identity;
    identity.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    identity.modified = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    identity.fileId = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
    return identity;
}
FileIdentity SourceLease::fingerprint(std::stop_token stop, const std::function<void(std::uint64_t,std::uint64_t)>& progress) {
    checkStop(stop);
    FileIdentity identity = metadata();
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle_,zero,nullptr,FILE_BEGIN)) throw std::runtime_error("Could not rewind source verification handle");
    Hash hash; hash.initialize();
    std::vector<UCHAR> buffer(1024 * 1024);
    std::uint64_t processed = 0;
    for (;;) {
        checkStop(stop);
        DWORD bytes = 0;
        if (!ReadFile(handle_,buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)) throw std::runtime_error("Could not read source for SHA-256 verification");
        if (!bytes) break;
        checkHash(BCryptHashData(hash.value,buffer.data(),bytes,0));
        processed += bytes;
        if (progress) progress(processed,identity.size);
    }
    if (processed != identity.size) throw std::runtime_error("Source size changed during verification");
    std::array<UCHAR,32> digest{};
    checkHash(BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0));
    checkStop(stop);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : digest) { identity.sha256 += hex[byte >> 4]; identity.sha256 += hex[byte & 15]; }
    return identity;
}
QueueOwner::QueueOwner(const std::filesystem::path& database) {
    const auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(database));
    const auto handle = CreateFileW(path.c_str(),GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Queue database cannot be opened for ownership (Windows error " + std::to_string(GetLastError()) + ")");
    // Lock a reserved byte far beyond EOF, outside SQLite's locking region.
    // A kernel range lock follows the file identity across hard-link aliases,
    // releases on process death, and does not extend or modify the database.
    OVERLAPPED position{}; position.Offset = 0xffff0000; position.OffsetHigh = 0x7fffffff;
    if (!LockFileEx(handle,LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&position)) {
        const auto error = GetLastError(); CloseHandle(handle);
        throw std::runtime_error("Queue already has an active owner or cannot be locked (Windows error " + std::to_string(error) + ")");
    }
    handle_ = handle;
}
QueueOwner::~QueueOwner() { if (handle_) CloseHandle(handle_); }
} // namespace rifelove::application

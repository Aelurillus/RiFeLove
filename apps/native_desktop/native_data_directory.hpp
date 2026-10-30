#pragma once

#include "native_model_store.hpp"

#include <shlobj.h>
#include <sqlite3.h>
#include <array>
#include <memory>

namespace rifelove::native_desktop {

struct NativeDataDirectory {
    std::filesystem::path directory;
    std::filesystem::path legacyDirectory;
    bool portable = false;
    bool writable = false;
    bool queueReady = false;
    bool preferencesReady = false;
    std::wstring notice;
};

inline bool nativeDataEntryExists(const std::filesystem::path& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    throw std::runtime_error("Cannot inspect native data entry");
}

inline std::filesystem::path nativeLocalAppDataRoot() {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path)))
        throw std::runtime_error("Cannot resolve local application data directory");
    return nativeModelPath(nativeModelUtf8(path));
}

inline std::filesystem::path uniqueNativeDataPath(const std::filesystem::path& directory, const wchar_t* prefix) {
    static std::atomic<unsigned long> sequence{0};
    return directory / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++));
}

inline void prepareWritableNativeDataDirectory(const std::filesystem::path& directory) {
    const auto path = nativeModelPath(nativeModelUtf8(std::filesystem::absolute(directory).lexically_normal().wstring()));
    // Validate the local drive and the nearest existing ancestor before any
    // directory creation; neither profiles nor portable mode follow junctions.
    (void)checkedNativeModelDirectory(path.root_path(), false);
    auto ancestor = path;
    while (!nativeDataEntryExists(ancestor)) ancestor = ancestor.parent_path();
    (void)checkedNativeModelDirectory(ancestor, false);
    std::filesystem::create_directories(path);
    (void)checkedNativeModelDirectory(path, false);
    const auto probe = uniqueNativeDataPath(path, L".write-probe-");
    const HANDLE handle = CreateFileW(probe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Native data directory is not writable");
    const char byte = '1';
    DWORD written = 0;
    const bool writable = WriteFile(handle, &byte, 1, &written, nullptr) && written == 1 && FlushFileBuffers(handle);
    CloseHandle(handle); // Deletes only this invocation's newly created probe.
    if (!writable) throw std::runtime_error("Cannot write native data directory");
}

class NativeDataReadLease final {
public:
    explicit NativeDataReadLease(const std::filesystem::path& path) {
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Legacy data is unreadable or in use");
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileType(handle_) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("Native data entry is not a local regular file");
        }
    }
    ~NativeDataReadLease() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    NativeDataReadLease(const NativeDataReadLease&) = delete;
    NativeDataReadLease& operator=(const NativeDataReadLease&) = delete;
    void copyTo(const std::filesystem::path& destination) const {
        const HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot stage native data migration");
        std::array<char, 65536> bytes{};
        bool copied = true;
        for (;;) {
            DWORD read = 0, written = 0;
            if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) { copied = false; break; }
            if (!read) break;
            if (!WriteFile(output, bytes.data(), read, &written, nullptr) || written != read) { copied = false; break; }
        }
        if (copied) copied = FlushFileBuffers(output) != FALSE;
        CloseHandle(output);
        if (!copied) throw std::runtime_error("Cannot copy legacy native data");
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

inline bool nativePortableModeRequested(const std::filesystem::path& applicationDirectory) {
    (void)checkedNativeModelDirectory(applicationDirectory, false);
    const auto marker = applicationDirectory / L"portable-mode.txt";
    if (!nativeDataEntryExists(marker)) return false;
    NativeDataReadLease lease(marker);
    if (std::filesystem::file_size(marker) > 128) throw std::runtime_error("Invalid portable-mode marker");
    std::ifstream input(marker, std::ios::binary);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
    if (line != "RifeLovePortable 1") throw std::runtime_error("Invalid portable-mode marker");
    return true;
}

struct NativeMigrationStage final {
    std::filesystem::path directory;
    explicit NativeMigrationStage(const std::filesystem::path& parent) : directory(uniqueNativeDataPath(parent, L".native-migration-")) {
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot create native migration stage");
    }
    ~NativeMigrationStage() {
        // This directory was created exclusively above. No legacy data or
        // existing destination entries are ever removed by migration cleanup.
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

inline void backupStagedNativeQueue(const std::filesystem::path& source, const std::filesystem::path& destination) {
    using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    const auto open = [](const std::filesystem::path& file, int flags) {
        sqlite3* raw = nullptr;
        const int code = sqlite3_open_v2(nativeModelUtf8(file.wstring()).c_str(), &raw, flags, nullptr);
        Database result(raw, &sqlite3_close);
        if (code != SQLITE_OK) throw std::runtime_error("Cannot open staged legacy queue");
        return result;
    };
    // Only the private copy is opened read/write: SQLite may recover its WAL
    // or rollback journal without touching the originals in Program Files.
    auto input = open(source, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX);
    auto output = open(destination, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX);
    sqlite3_backup* backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
    if (!backup) throw std::runtime_error("Cannot initialize native queue backup");
    const int copied = sqlite3_backup_step(backup, -1);
    const int finished = sqlite3_backup_finish(backup);
    if (copied != SQLITE_DONE || finished != SQLITE_OK) throw std::runtime_error("Cannot finish native queue backup");
    if (sqlite3_exec(output.get(), "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot finalize self-contained native queue backup");
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(output.get(), "PRAGMA quick_check", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot verify migrated native queue");
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> check(raw, &sqlite3_finalize);
    if (sqlite3_step(check.get()) != SQLITE_ROW || !sqlite3_column_text(check.get(), 0) ||
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(check.get(), 0))) != "ok" ||
        sqlite3_step(check.get()) != SQLITE_DONE)
        throw std::runtime_error("Migrated native queue failed integrity verification");
}

inline void migrateNativeQueue(const std::filesystem::path& legacy, const std::filesystem::path& destination) {
    const auto target = destination / L"queue.sqlite";
    if (nativeDataEntryExists(target)) { NativeDataReadLease existing(target); return; }
    for (const auto* suffix : {L"queue.sqlite-wal", L"queue.sqlite-journal", L"queue.sqlite-shm"})
        if (nativeDataEntryExists(destination / suffix)) throw std::runtime_error("Destination queue has orphaned sidecars");
    (void)checkedNativeModelDirectory(legacy.parent_path(), false);
    if (!nativeDataEntryExists(legacy)) return;
    (void)checkedNativeModelDirectory(legacy, false);
    const auto source = legacy / L"queue.sqlite";
    if (!nativeDataEntryExists(source)) {
        if (nativeDataEntryExists(legacy / L"queue.sqlite-wal") || nativeDataEntryExists(legacy / L"queue.sqlite-journal"))
            throw std::runtime_error("Legacy queue sidecars exist without their database");
        return;
    }
    // Deny concurrent writers/replacement while taking a byte-stable snapshot.
    // An active Native queue already holds a read/write handle, so the lease
    // fails instead of migrating a database that still has an active owner.
    NativeDataReadLease database(source);
    std::unique_ptr<NativeDataReadLease> wal, journal;
    if (nativeDataEntryExists(legacy / L"queue.sqlite-wal")) wal = std::make_unique<NativeDataReadLease>(legacy / L"queue.sqlite-wal");
    if (nativeDataEntryExists(legacy / L"queue.sqlite-journal")) journal = std::make_unique<NativeDataReadLease>(legacy / L"queue.sqlite-journal");
    NativeMigrationStage stage(destination);
    database.copyTo(stage.directory / L"source.sqlite");
    if (wal) wal->copyTo(stage.directory / L"source.sqlite-wal");
    if (journal) journal->copyTo(stage.directory / L"source.sqlite-journal");
    const auto snapshot = stage.directory / L"queue.sqlite";
    backupStagedNativeQueue(stage.directory / L"source.sqlite", snapshot);
    // Publication never replaces a destination that another launch created.
    if (!MoveFileExW(snapshot.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (!nativeDataEntryExists(target)) throw std::runtime_error("Cannot publish migrated native queue");
        NativeDataReadLease existing(target);
    }
}

inline void migrateNativePreferences(const std::filesystem::path& legacy, const std::filesystem::path& destination) {
    const auto target = destination / L"local-models.txt";
    if (nativeDataEntryExists(target)) { NativeDataReadLease existing(target); return; }
    (void)checkedNativeModelDirectory(legacy.parent_path(), false);
    if (!nativeDataEntryExists(legacy)) return;
    (void)checkedNativeModelDirectory(legacy, false);
    const auto source = legacy / L"local-models.txt";
    if (!nativeDataEntryExists(source)) return;
    NativeDataReadLease lease(source);
    const auto preferences = loadNativeModelPreferences(source);
    try { saveNativeModelPreferences(target, preferences, false); }
    catch (...) {
        if (!nativeDataEntryExists(target)) throw;
        NativeDataReadLease existing(target);
    }
}

inline NativeDataDirectory prepareNativeDataDirectory(const std::filesystem::path& applicationDirectory,
    const std::filesystem::path& localAppDataRoot) {
    NativeDataDirectory result;
    result.legacyDirectory = applicationDirectory / L"data";
    result.directory = localAppDataRoot / L"RifeLove";
    try {
        if (nativePortableModeRequested(applicationDirectory)) {
            try {
                prepareWritableNativeDataDirectory(result.legacyDirectory);
                result.directory = result.legacyDirectory;
                result.portable = true;
                result.notice = L"便携模式：数据保存在程序的 data 目录。";
            } catch (const std::exception&) {
                result.notice = L"便携数据目录不可写，改用本机用户数据目录；旧数据保留。";
            }
        }
    } catch (const std::exception&) {
        result.notice = L"便携标记无法读取或格式无效，使用本机用户数据目录。";
    }
    try {
        prepareWritableNativeDataDirectory(result.directory);
        result.writable = true;
    } catch (const std::exception&) {
        result.notice += L" 用户数据目录不可写；模型设置与队列已停用，原数据未改动。";
        return result;
    }
    try { migrateNativeQueue(result.legacyDirectory, result.directory); result.queueReady = true; }
    catch (const std::exception&) { result.notice += L" 旧队列无法安全迁移（可能仍被占用）；保留原件，暂不新建队列。"; }
    try { migrateNativePreferences(result.legacyDirectory, result.directory); result.preferencesReady = true; }
    catch (const std::exception&) { result.notice += L" 旧模型设置无法安全迁移；保留原件，暂不写入新设置。"; }
    if (!result.portable && result.notice.empty()) result.notice = L"使用本机用户数据目录；旧版 data 中的原件保留，现有用户数据优先。";
    return result;
}

} // namespace rifelove::native_desktop

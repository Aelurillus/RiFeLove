#pragma once

#include "native_data_directory.hpp"
#include "native_model_center_test.hpp"

#include <iterator>

namespace rifelove::native_test {

using NativeTestDatabase = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

inline NativeTestDatabase openNativeTestDatabase(const std::filesystem::path& file, int flags) {
    sqlite3* raw = nullptr;
    const int code = sqlite3_open_v2(native_desktop::nativeModelUtf8(file.wstring()).c_str(), &raw, flags, nullptr);
    NativeTestDatabase result(raw, &sqlite3_close);
    if (code != SQLITE_OK) throw std::runtime_error("Cannot open temporary native data fixture");
    return result;
}

inline void executeNativeTestSql(sqlite3* database, const char* sql) {
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot prepare temporary native queue fixture");
}

inline std::string nativeTestBytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read temporary native data fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::string nativeQueueSentinel(const std::filesystem::path& file) {
    const auto database = openNativeTestDatabase(file, SQLITE_OPEN_READONLY);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database.get(), "SELECT value FROM migration_sentinel", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Migrated queue lost its fixture row");
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, &sqlite3_finalize);
    if (sqlite3_step(statement.get()) != SQLITE_ROW || !sqlite3_column_text(statement.get(), 0))
        throw std::runtime_error("Migrated queue fixture row is unreadable");
    return reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
}

inline void makeNativeQueueFixture(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    const auto database = openNativeTestDatabase(file, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    executeNativeTestSql(database.get(), "CREATE TABLE migration_sentinel(value TEXT); INSERT INTO migration_sentinel VALUES('legacy');");
}

struct NativeTestExclusiveFile final {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit NativeTestExclusiveFile(const std::filesystem::path& file) {
        handle = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock temporary native data fixture");
    }
    ~NativeTestExclusiveFile() { CloseHandle(handle); }
};

template<class Check>
void checkNativeDataDirectories(Check check) {
    using namespace native_desktop;
    ModelCenterTemporaryDirectory temporary;
    const auto app = temporary.root / L"Program Files" / L"RifeLove";
    const auto legacy = app / L"data";
    const auto local = temporary.root / L"用户 LocalAppData";
    const auto destination = local / L"RifeLove";
    std::filesystem::create_directories(legacy);

    // Copy a committed, uncheckpointed WAL fixture into the legacy directory.
    // The live writer belongs only to this separate temporary seed database.
    const auto seed = temporary.root / L"wal-seed.sqlite";
    {
        const auto writer = openNativeTestDatabase(seed, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        executeNativeTestSql(writer.get(), "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE migration_sentinel(value TEXT); INSERT INTO migration_sentinel VALUES('legacy');");
        std::filesystem::copy_file(seed, legacy / L"queue.sqlite");
        std::filesystem::copy_file(std::filesystem::path(seed.wstring() + L"-wal"), legacy / L"queue.sqlite-wal");
    }
    NativeModelPreferences preferences;
    const auto external = temporary.root / L"外部 模型";
    preferences.registerDirectory(external);
    preferences.defaults[0] = external;
    saveNativeModelPreferences(legacy / L"local-models.txt", preferences);
    const auto oldQueue = nativeTestBytes(legacy / L"queue.sqlite");
    const auto oldWal = nativeTestBytes(legacy / L"queue.sqlite-wal");
    const auto oldPreferences = nativeTestBytes(legacy / L"local-models.txt");

    const auto installed = prepareNativeDataDirectory(app, local);
    check(installed.directory == destination && !installed.portable && installed.writable && installed.queueReady && installed.preferencesReady,
        "installed mode uses writable LocalAppData even when the executable directory is writable");
    check(nativeQueueSentinel(destination / L"queue.sqlite") == "legacy",
        "legacy queue migration includes committed WAL rows in a self-contained SQLite backup");
    check(loadNativeModelPreferences(destination / L"local-models.txt").defaults[0] == external,
        "installed mode migrates legacy Unicode model preferences");
    check(nativeTestBytes(legacy / L"queue.sqlite") == oldQueue && nativeTestBytes(legacy / L"queue.sqlite-wal") == oldWal &&
        nativeTestBytes(legacy / L"local-models.txt") == oldPreferences,
        "migration leaves the legacy database, WAL and settings byte-for-byte unchanged");

    {
        const auto current = openNativeTestDatabase(destination / L"queue.sqlite", SQLITE_OPEN_READWRITE);
        executeNativeTestSql(current.get(), "UPDATE migration_sentinel SET value='current';");
    }
    const auto newerModel = temporary.root / L"newer-default";
    auto newerPreferences = preferences;
    newerPreferences.defaults[0] = newerModel;
    saveNativeModelPreferences(destination / L"local-models.txt", newerPreferences);
    const auto existing = prepareNativeDataDirectory(app, local);
    check(existing.queueReady && existing.preferencesReady && nativeQueueSentinel(destination / L"queue.sqlite") == "current" &&
        loadNativeModelPreferences(destination / L"local-models.txt").defaults[0] == newerModel,
        "existing user queue and preferences always win without merging or overwriting them from legacy data");

    const auto portableApp = temporary.root / L"Portable";
    std::filesystem::create_directories(portableApp / L"data");
    writeModelCenterText(portableApp / L"portable-mode.txt", "RifeLovePortable 1\r\n");
    makeNativeQueueFixture(portableApp / L"data" / L"queue.sqlite");
    saveNativeModelPreferences(portableApp / L"data" / L"local-models.txt", preferences);
    const auto portableQueue = nativeTestBytes(portableApp / L"data" / L"queue.sqlite");
    const auto portableSettings = nativeTestBytes(portableApp / L"data" / L"local-models.txt");
    const auto portable = prepareNativeDataDirectory(portableApp, temporary.root / L"UnusedLocalAppData");
    check(portable.portable && portable.directory == portableApp / L"data" && portable.writable && portable.queueReady && portable.preferencesReady,
        "explicit writable portable mode uses its existing adjacent data directory");
    check(nativeTestBytes(portableApp / L"data" / L"queue.sqlite") == portableQueue &&
        nativeTestBytes(portableApp / L"data" / L"local-models.txt") == portableSettings &&
        !std::filesystem::exists(temporary.root / L"UnusedLocalAppData" / L"RifeLove"),
        "portable selection preserves existing files and does not create a separate user profile");

    const auto blockedPortable = temporary.root / L"BlockedPortable";
    std::filesystem::create_directories(blockedPortable);
    writeModelCenterText(blockedPortable / L"portable-mode.txt", "RifeLovePortable 1\n");
    writeModelCenterText(blockedPortable / L"data", "existing blocker must survive");
    const auto fallbackRoot = temporary.root / L"FallbackLocalAppData";
    const auto fallback = prepareNativeDataDirectory(blockedPortable, fallbackRoot);
    check(!fallback.portable && fallback.writable && fallback.directory == fallbackRoot / L"RifeLove" && !fallback.notice.empty() &&
        nativeTestBytes(blockedPortable / L"data") == "existing blocker must survive",
        "unusable portable directory falls back explicitly without replacing the original data entry");

    const auto blockedRoot = temporary.root / L"BlockedLocalAppData";
    std::filesystem::create_directory(blockedRoot);
    writeModelCenterText(blockedRoot / L"RifeLove", "profile blocker");
    const auto unavailable = prepareNativeDataDirectory(app, blockedRoot);
    check(!unavailable.writable && !unavailable.queueReady && !unavailable.preferencesReady &&
        nativeTestBytes(blockedRoot / L"RifeLove") == "profile blocker" && nativeTestBytes(legacy / L"queue.sqlite") == oldQueue,
        "unwritable user-data target disables persistence without falling back to an empty executable-directory queue");

    const auto lockedApp = temporary.root / L"LockedSource";
    makeNativeQueueFixture(lockedApp / L"data" / L"queue.sqlite");
    saveNativeModelPreferences(lockedApp / L"data" / L"local-models.txt", preferences);
    const auto lockedLocal = temporary.root / L"LockedLocalAppData";
    const auto lockedQueueBefore = nativeTestBytes(lockedApp / L"data" / L"queue.sqlite");
    const auto lockedPreferencesBefore = nativeTestBytes(lockedApp / L"data" / L"local-models.txt");
    {
        NativeTestExclusiveFile queueLock(lockedApp / L"data" / L"queue.sqlite");
        NativeTestExclusiveFile preferenceLock(lockedApp / L"data" / L"local-models.txt");
        const auto blocked = prepareNativeDataDirectory(lockedApp, lockedLocal);
        check(blocked.writable && !blocked.queueReady && !blocked.preferencesReady &&
            !std::filesystem::exists(lockedLocal / L"RifeLove" / L"queue.sqlite") &&
            !std::filesystem::exists(lockedLocal / L"RifeLove" / L"local-models.txt"),
            "unreadable or actively owned legacy data never produces replacement empty files");
    }
    check(nativeTestBytes(lockedApp / L"data" / L"queue.sqlite") == lockedQueueBefore &&
        nativeTestBytes(lockedApp / L"data" / L"local-models.txt") == lockedPreferencesBefore,
        "failed legacy reads leave every original byte unchanged");
    const auto retried = prepareNativeDataDirectory(lockedApp, lockedLocal);
    check(retried.queueReady && retried.preferencesReady && nativeQueueSentinel(lockedLocal / L"RifeLove" / L"queue.sqlite") == "legacy",
        "safe migration can retry after the legacy owner releases its files");

    const auto corruptApp = temporary.root / L"CorruptLegacy";
    std::filesystem::create_directories(corruptApp / L"data");
    writeModelCenterText(corruptApp / L"data" / L"queue.sqlite", "not a SQLite database");
    writeModelCenterText(corruptApp / L"data" / L"local-models.txt", "unrecognized user settings");
    const auto corruptLocal = temporary.root / L"CorruptLocalAppData";
    const auto corrupt = prepareNativeDataDirectory(corruptApp, corruptLocal);
    check(!corrupt.queueReady && !corrupt.preferencesReady &&
        !std::filesystem::exists(corruptLocal / L"RifeLove" / L"queue.sqlite") &&
        !std::filesystem::exists(corruptLocal / L"RifeLove" / L"local-models.txt") &&
        nativeTestBytes(corruptApp / L"data" / L"queue.sqlite") == "not a SQLite database" &&
        nativeTestBytes(corruptApp / L"data" / L"local-models.txt") == "unrecognized user settings",
        "invalid legacy databases and settings are retained rather than converted into fresh defaults");

    const auto targetSettings = destination / L"local-models.txt";
    const auto preservedSettings = nativeTestBytes(targetSettings);
    bool writeFailed = false;
    {
        NativeTestExclusiveFile lock(targetSettings);
        try { saveNativeModelPreferences(targetSettings, preferences); } catch (const std::exception&) { writeFailed = true; }
    }
    check(writeFailed && nativeTestBytes(targetSettings) == preservedSettings && nativeTestBytes(legacy / L"local-models.txt") == oldPreferences,
        "a failed settings replacement preserves both current user settings and the legacy original");
}

} // namespace rifelove::native_test

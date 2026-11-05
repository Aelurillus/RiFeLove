#pragma once

#include "native_package_archive.hpp"
#include "native_model_center_test.hpp"

#include <miniz.h>

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <stop_token>
#include <utility>

namespace rifelove::native_test {

inline std::string nativePackageTestSha256(std::span<const unsigned char> bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot open SHA-256 for package fixture");
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot create SHA-256 for package fixture");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ULONG count = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        if (BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + offset), count, 0) < 0) {
            BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("Cannot hash package fixture");
        }
        offset += count;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot finish package fixture hash");
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : digest) { output += digits[byte >> 4]; output += digits[byte & 15]; }
    return output;
}

inline std::string nativePackageTestSha256(const std::string& value) {
    return nativePackageTestSha256(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(value.data()), value.size()));
}

inline std::string nativePackageTestFileSha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.eof() && !input) throw std::runtime_error("Cannot read package fixture archive");
    return nativePackageTestSha256(bytes);
}

inline bool writeNativePackageFixture(const std::filesystem::path& path,
    const std::vector<std::pair<std::string, std::string>>& entries) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    mz_zip_archive archive{};
    bool initialized = mz_zip_writer_init_cfile(&archive, file, 0) != 0;
    bool written = initialized;
    for (const auto& [name, data] : entries) {
        if (!written) break;
        written = mz_zip_writer_add_mem(&archive, name.c_str(), data.data(), data.size(), MZ_NO_COMPRESSION) != 0;
    }
    if (written) written = mz_zip_writer_finalize_archive(&archive) != 0;
    if (initialized) (void)mz_zip_writer_end(&archive);
    std::fclose(file);
    return written;
}

template<class Check>
void checkNativeModelPackageArchive(Check check) {
    using namespace native_desktop;
    ModelCenterTemporaryDirectory temporary;
    const std::string modelContents = "fixture model descriptor";
    const std::vector<std::pair<std::string, std::string>> validEntries{
        {"README.txt", "fixture readme"},
        {"rife-fixture/model.json", modelContents}
    };
    const auto archive = temporary.root / L"fixture-package.zip";
    const bool archiveCreated = writeNativePackageFixture(archive, validEntries);
    check(archiveCreated, "create a local model package archive fixture");
    if (!archiveCreated) return;

    NativePackageArchivePlan plan;
    plan.rootDirectory = "rife-fixture";
    plan.archiveSizeBytes = std::filesystem::file_size(archive);
    plan.archiveSha256 = nativePackageTestFileSha256(archive);
    plan.files = {{"model.json", modelContents.size(), nativePackageTestSha256(modelContents)}};
    plan.maximumUncompressedBytes = 128 * 1024;
    const auto staging = temporary.root / L"staged-install";
    bool extracted = false;
    try {
        const auto modelDirectory = extractNativeModelPackageArchive(archive, plan, staging);
        std::ifstream input(modelDirectory / L"model.json", std::ios::binary);
        std::string observed((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        extracted = modelDirectory == staging / L"rife-fixture" && observed == modelContents &&
            std::filesystem::exists(staging / L"README.txt");
    } catch (...) { }
    check(extracted, "extract only manifest-listed files into a fresh staging directory");

    const auto mismatchStage = temporary.root / L"mismatched-size-stage";
    auto mismatch = plan;
    ++mismatch.files.front().sizeBytes;
    bool mismatchRejected = false;
    try { (void)extractNativeModelPackageArchive(archive, mismatch, mismatchStage); }
    catch (const std::exception&) { mismatchRejected = true; }
    check(mismatchRejected && !std::filesystem::exists(mismatchStage),
        "reject manifest size mismatches and remove partial staging data");

    const auto archiveHashStage = temporary.root / L"mismatched-archive-hash-stage";
    auto badArchiveHash = plan;
    badArchiveHash.archiveSha256[0] = badArchiveHash.archiveSha256[0] == '0' ? '1' : '0';
    bool archiveHashRejected = false;
    try { (void)extractNativeModelPackageArchive(archive, badArchiveHash, archiveHashStage); }
    catch (const std::exception&) { archiveHashRejected = true; }
    check(archiveHashRejected && !std::filesystem::exists(archiveHashStage),
        "reject an archive checksum mismatch before creating staging data");

    const auto fileHashStage = temporary.root / L"mismatched-file-hash-stage";
    auto badFileHash = plan;
    badFileHash.files.front().sha256[0] = badFileHash.files.front().sha256[0] == '0' ? '1' : '0';
    bool fileHashRejected = false;
    try { (void)extractNativeModelPackageArchive(archive, badFileHash, fileHashStage); }
    catch (const std::exception&) { fileHashRejected = true; }
    check(fileHashRejected && !std::filesystem::exists(fileHashStage),
        "reject a model file checksum mismatch and remove extracted data");

    const auto traversalArchive = temporary.root / L"path-traversal.zip";
    const auto traversalEntries = std::vector<std::pair<std::string, std::string>>{
        {"README.txt", "fixture readme"},
        {"rife-fixture/../outside.txt", "must not escape"}
    };
    const auto traversalCreated = writeNativePackageFixture(traversalArchive, traversalEntries);
    bool traversalRejected = false;
    const auto traversalStage = temporary.root / L"path-traversal-stage";
    try { (void)extractNativeModelPackageArchive(traversalArchive, plan, traversalStage); }
    catch (const std::exception&) { traversalRejected = true; }
    check(traversalCreated && traversalRejected && !std::filesystem::exists(traversalStage) &&
        !std::filesystem::exists(temporary.root / L"outside.txt"),
        "reject path traversal and clean the isolated staging directory");

    const auto duplicateArchive = temporary.root / L"duplicate-entry.zip";
    const auto duplicateEntries = std::vector<std::pair<std::string, std::string>>{
        {"README.txt", "fixture readme"},
        {"rife-fixture/model.json", modelContents},
        {"rife-fixture/model.json", "duplicate"}
    };
    const auto duplicateCreated = writeNativePackageFixture(duplicateArchive, duplicateEntries);
    bool duplicateRejected = false;
    const auto duplicateStage = temporary.root / L"duplicate-stage";
    try { (void)extractNativeModelPackageArchive(duplicateArchive, plan, duplicateStage); }
    catch (const std::exception&) { duplicateRejected = true; }
    check(duplicateCreated && duplicateRejected && !std::filesystem::exists(duplicateStage),
        "reject duplicate archive paths without leaving partial files");

    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelledStage = temporary.root / L"cancelled-stage";
    bool cancelled = false;
    try { (void)extractNativeModelPackageArchive(archive, plan, cancelledStage, cancellation.get_token()); }
    catch (const NativePackageInstallCancelled&) { cancelled = true; }
    check(cancelled && !std::filesystem::exists(cancelledStage),
        "honor cancellation before extraction without creating installation data");

    bool unsafePlanRejected = false;
    auto unsafePlan = plan;
    unsafePlan.rootDirectory = "../outside";
    try { (void)extractNativeModelPackageArchive(archive, unsafePlan, temporary.root / L"unsafe-plan-stage"); }
    catch (const std::invalid_argument&) { unsafePlanRejected = true; }
    check(unsafePlanRejected, "reject an unsafe root path before creating staging data");
}

} // namespace rifelove::native_test

#include "media/nvenc_diagnostic.hpp"

#include "nvenc_build_config.hpp"

#include <bit>
#include <cstdint>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rifelove::media {
namespace {
bool isNvencEncoder(std::string_view name) {
    return name == "h264_nvenc" || name == "hevc_nvenc" || name == "av1_nvenc";
}

bool lessThan(NvencApiVersion left, NvencApiVersion right) {
    return left.major < right.major || (left.major == right.major && left.minor < right.minor);
}
} // namespace

std::optional<NvencApiVersion> queryNvidiaNvencApiVersion() {
#if defined(_WIN32)
    using QueryMaxVersion = int(__stdcall*)(std::uint32_t*);
    struct LibraryGuard {
        HMODULE handle;
        ~LibraryGuard() { if (handle) FreeLibrary(handle); }
    } library{LoadLibraryExW(L"nvEncodeAPI64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!library.handle) return std::nullopt;
    const auto rawQuery = GetProcAddress(library.handle,"NvEncodeAPIGetMaxSupportedVersion");
    static_assert(sizeof(QueryMaxVersion) == sizeof(FARPROC));
    const auto query = std::bit_cast<QueryMaxVersion>(rawQuery);
    if (!query) {
        return std::nullopt;
    }
    std::uint32_t encodedVersion = 0;
    const auto status = query(&encodedVersion);
    if (status != 0) return std::nullopt;
    return NvencApiVersion{(encodedVersion >> 4U) & 0xffU,encodedVersion & 0x0fU};
#else
    return std::nullopt;
#endif
}

std::optional<NvencApiMismatch> findNvencApiMismatch(
    std::optional<NvencApiVersion> available, NvencApiVersion required, std::string minimumDriver) {
    if (!available || !lessThan(*available,required)) return std::nullopt;
    return NvencApiMismatch{*available,required,std::move(minimumDriver)};
}

std::string nvencApiMismatchReason(const NvencApiMismatch& mismatch) {
    return "Driver does not support required nvenc API version. Required: " +
        std::to_string(mismatch.required.major) + "." + std::to_string(mismatch.required.minor) +
        " Found: " + std::to_string(mismatch.available.major) + "." + std::to_string(mismatch.available.minor) +
        "\nThe minimum required Nvidia driver for nvenc is " + mismatch.minimumDriver + " or newer.";
}

std::string diagnoseNvencFailure(const std::string& encoder, const std::string& reason) {
    if (!isNvencEncoder(encoder)) return reason;
    const auto mismatch = findNvencApiMismatch(queryNvidiaNvencApiVersion(),
        {RIFELOVE_NVENC_API_MAJOR,RIFELOVE_NVENC_API_MINOR},RIFELOVE_NVENC_MINIMUM_WINDOWS_DRIVER);
    return mismatch ? nvencApiMismatchReason(*mismatch) : reason;
}

} // namespace rifelove::media

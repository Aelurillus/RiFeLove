#pragma once

#include <optional>
#include <string>

namespace rifelove::media {

struct NvencApiVersion {
    unsigned int major = 0;
    unsigned int minor = 0;
};

struct NvencApiMismatch {
    NvencApiVersion available;
    NvencApiVersion required;
    std::string minimumDriver;
};

std::optional<NvencApiVersion> queryNvidiaNvencApiVersion();
std::optional<NvencApiMismatch> findNvencApiMismatch(
    std::optional<NvencApiVersion> available, NvencApiVersion required, std::string minimumDriver);
std::string nvencApiMismatchReason(const NvencApiMismatch& mismatch);
std::string diagnoseNvencFailure(const std::string& encoder, const std::string& reason);

} // namespace rifelove::media

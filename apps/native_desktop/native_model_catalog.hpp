#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace rifelove::native_desktop {

struct NativeModelCatalogTrust final {
    std::string keyId;
    // Uncompressed P-256 point coordinates, X followed by Y, 32 bytes each.
    std::array<std::uint8_t, 64> publicPoint{};
};

// The payload is returned only after its detached P-256/SHA-256 signature is
// verified against the caller-supplied trust root. The envelope is bounded and
// has a strict line format so parsing cannot disagree about signed bytes.
[[nodiscard]] std::string verifyNativeSignedModelCatalog(
    std::string_view envelope,
    const NativeModelCatalogTrust& trust);

[[nodiscard]] const NativeModelCatalogTrust& officialNativeModelCatalogTrust();
[[nodiscard]] std::string verifyOfficialNativeModelCatalog(std::string_view envelope);

} // namespace rifelove::native_desktop

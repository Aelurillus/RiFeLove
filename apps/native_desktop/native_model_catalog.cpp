#include "native_model_catalog.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace rifelove::native_desktop {
namespace {
constexpr std::size_t kMaximumEnvelopeBytes = 384 * 1024;
constexpr std::size_t kMaximumPayloadBytes = 256 * 1024;
constexpr std::string_view kEnvelopeHeader = "RIFELOVE MODEL CATALOG 1";

class AlgorithmHandle final {
public:
    explicit AlgorithmHandle(LPCWSTR algorithm) {
        if (BCryptOpenAlgorithmProvider(&value_, algorithm, nullptr, 0) < 0)
            throw std::runtime_error("Cannot initialize model catalog cryptography");
    }
    ~AlgorithmHandle() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class HashHandle final {
public:
    explicit HashHandle(BCRYPT_ALG_HANDLE algorithm) {
        DWORD objectBytes = 0;
        DWORD written = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &written, 0) < 0 ||
            written != sizeof(objectBytes) || objectBytes == 0)
            throw std::runtime_error("Cannot initialize model catalog hashing");
        object_.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &value_, object_.data(), objectBytes, nullptr, 0, 0) < 0)
            throw std::runtime_error("Cannot initialize model catalog hashing");
    }
    ~HashHandle() { if (value_) BCryptDestroyHash(value_); }
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
private:
    std::vector<UCHAR> object_;
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

class KeyHandle final {
public:
    ~KeyHandle() { if (value_) BCryptDestroyKey(value_); }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;
    KeyHandle() = default;
    [[nodiscard]] BCRYPT_KEY_HANDLE* address() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return value_; }
private:
    BCRYPT_KEY_HANDLE value_ = nullptr;
};

std::vector<std::string_view> envelopeLines(std::string_view envelope) {
    if (envelope.empty() || envelope.size() > kMaximumEnvelopeBytes)
        throw std::invalid_argument("Signed model catalog exceeds its size limit");
    std::vector<std::string_view> lines;
    lines.reserve(4);
    std::size_t offset = 0;
    while (offset < envelope.size()) {
        const auto end = envelope.find('\n', offset);
        auto line = envelope.substr(offset, end == std::string_view::npos ? envelope.size() - offset : end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find('\r') != std::string_view::npos)
            throw std::invalid_argument("Signed model catalog has invalid line endings");
        lines.push_back(line);
        if (lines.size() > 4) throw std::invalid_argument("Signed model catalog has extra fields");
        if (end == std::string_view::npos) break;
        offset = end + 1;
        if (offset == envelope.size()) break;
    }
    if (lines.size() != 4 || lines[0] != kEnvelopeHeader ||
        !lines[1].starts_with("key-id=") || !lines[2].starts_with("payload=") ||
        !lines[3].starts_with("signature="))
        throw std::invalid_argument("Unsupported signed model catalog envelope");
    return lines;
}

int base64Value(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::vector<UCHAR> decodeBase64(std::string_view encoded, std::size_t expectedBytes, std::size_t maximumBytes) {
    if (encoded.empty() || encoded.size() % 4 != 0 || encoded.size() > ((maximumBytes + 2) / 3) * 4)
        throw std::invalid_argument("Signed model catalog contains invalid base64 data");
    std::vector<UCHAR> output;
    output.reserve((encoded.size() / 4) * 3);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
        const bool last = offset + 4 == encoded.size();
        const int a = base64Value(encoded[offset]);
        const int b = base64Value(encoded[offset + 1]);
        if (a < 0 || b < 0) throw std::invalid_argument("Signed model catalog contains invalid base64 data");
        const char third = encoded[offset + 2];
        const char fourth = encoded[offset + 3];
        if (third == '=') {
            if (!last || fourth != '=' || (b & 15) != 0)
                throw std::invalid_argument("Signed model catalog contains noncanonical base64 data");
            output.push_back(static_cast<UCHAR>((a << 2) | (b >> 4)));
            continue;
        }
        const int c = base64Value(third);
        if (c < 0) throw std::invalid_argument("Signed model catalog contains invalid base64 data");
        output.push_back(static_cast<UCHAR>((a << 2) | (b >> 4)));
        output.push_back(static_cast<UCHAR>((b << 4) | (c >> 2)));
        if (fourth == '=') {
            if (!last || (c & 3) != 0)
                throw std::invalid_argument("Signed model catalog contains noncanonical base64 data");
            continue;
        }
        const int d = base64Value(fourth);
        if (d < 0) throw std::invalid_argument("Signed model catalog contains invalid base64 data");
        output.push_back(static_cast<UCHAR>((c << 6) | d));
    }
    if ((expectedBytes != 0 && output.size() != expectedBytes) || output.empty() || output.size() > maximumBytes)
        throw std::invalid_argument("Signed model catalog contains an unexpected field size");
    return output;
}

std::array<UCHAR, 32> sha256(std::string_view payload) {
    AlgorithmHandle algorithm(BCRYPT_SHA256_ALGORITHM);
    HashHandle hash(algorithm.get());
    if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(payload.data())),
            static_cast<ULONG>(payload.size()), 0) < 0)
        throw std::runtime_error("Cannot hash the signed model catalog");
    std::array<UCHAR, 32> digest{};
    if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish hashing the signed model catalog");
    return digest;
}

} // namespace

std::string verifyNativeSignedModelCatalog(std::string_view envelope, const NativeModelCatalogTrust& trust) {
    if (trust.keyId.empty() || trust.keyId.size() > 64 ||
        !std::all_of(trust.keyId.begin(), trust.keyId.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        }))
        throw std::invalid_argument("Invalid model catalog trust key");
    const auto lines = envelopeLines(envelope);
    const auto envelopeKeyId = lines[1].substr(std::string_view("key-id=").size());
    if (envelopeKeyId != trust.keyId)
        throw std::invalid_argument("Signed model catalog key ID is not trusted");
    const auto payloadBytes = decodeBase64(lines[2].substr(std::string_view("payload=").size()), 0, kMaximumPayloadBytes);
    const auto signature = decodeBase64(lines[3].substr(std::string_view("signature=").size()), 64, 64);
    const std::string payload(reinterpret_cast<const char*>(payloadBytes.data()), payloadBytes.size());
    const auto digest = sha256(payload);

    AlgorithmHandle algorithm(BCRYPT_ECDSA_P256_ALGORITHM);
    BCRYPT_ECCKEY_BLOB publicHeader{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
    std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + 64> publicBlob{};
    std::memcpy(publicBlob.data(), &publicHeader, sizeof(publicHeader));
    std::memcpy(publicBlob.data() + sizeof(publicHeader), trust.publicPoint.data(), trust.publicPoint.size());
    KeyHandle publicKey;
    if (BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, publicKey.address(),
            publicBlob.data(), static_cast<ULONG>(publicBlob.size()), 0) < 0)
        throw std::invalid_argument("Cannot import the model catalog trust key");
    if (BCryptVerifySignature(publicKey.get(), nullptr, const_cast<PUCHAR>(digest.data()),
            static_cast<ULONG>(digest.size()), const_cast<PUCHAR>(signature.data()),
            static_cast<ULONG>(signature.size()), 0) < 0)
        throw std::invalid_argument("Signed model catalog signature is invalid");
    return payload;
}

const NativeModelCatalogTrust& officialNativeModelCatalogTrust() {
    static const NativeModelCatalogTrust trust{
        "rifelove-model-catalog-2026-v1",
        {0x10, 0xca, 0xa8, 0xc4, 0x8b, 0x21, 0xb1, 0x59,
         0xd9, 0xd6, 0x44, 0x50, 0x71, 0x52, 0xf5, 0x77,
         0xba, 0x8e, 0xbd, 0xea, 0xa9, 0x2c, 0xb8, 0x22,
         0xe6, 0x84, 0x19, 0x74, 0x45, 0xa7, 0xf6, 0x23,
         0x0e, 0xc1, 0xe2, 0x1d, 0x1e, 0xf1, 0x44, 0xe9,
         0x6c, 0x91, 0x6d, 0xb4, 0x92, 0xe7, 0x2e, 0xeb,
         0x50, 0xdf, 0x14, 0x7f, 0x79, 0x00, 0xee, 0x22,
         0x87, 0x73, 0xc3, 0xf6, 0x77, 0x39, 0x19, 0x85}
    };
    return trust;
}

std::string verifyOfficialNativeModelCatalog(std::string_view envelope) {
    return verifyNativeSignedModelCatalog(envelope, officialNativeModelCatalogTrust());
}

} // namespace rifelove::native_desktop

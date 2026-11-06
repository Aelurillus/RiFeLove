#pragma once

#include "native_model_catalog.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace rifelove::native_test {

inline std::string nativeCatalogTestBase64(const UCHAR* bytes, std::size_t length) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((length + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < length; offset += 3) {
        const auto remaining = length - offset;
        const unsigned int first = bytes[offset];
        const unsigned int second = remaining > 1 ? bytes[offset + 1] : 0;
        const unsigned int third = remaining > 2 ? bytes[offset + 2] : 0;
        encoded.push_back(alphabet[first >> 2]);
        encoded.push_back(alphabet[((first & 3U) << 4) | (second >> 4)]);
        encoded.push_back(remaining > 1 ? alphabet[((second & 15U) << 2) | (third >> 6)] : '=');
        encoded.push_back(remaining > 2 ? alphabet[third & 63U] : '=');
    }
    return encoded;
}

inline std::array<UCHAR, 32> nativeCatalogTestDigest(std::string_view value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot open SHA-256 for catalog fixture");
    DWORD objectBytes = 0, written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
            sizeof(objectBytes), &written, 0) < 0 || written != sizeof(objectBytes) || objectBytes == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot prepare SHA-256 for catalog fixture");
    }
    std::vector<UCHAR> object(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot create SHA-256 for catalog fixture");
    }
    const auto status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
        static_cast<ULONG>(value.size()), 0);
    std::array<UCHAR, 32> digest{};
    const auto finishStatus = status < 0 ? status : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finishStatus < 0) throw std::runtime_error("Cannot finish SHA-256 for catalog fixture");
    return digest;
}

template<class Check>
void checkNativeSignedModelCatalog(Check check) {
    using namespace native_desktop;
    constexpr std::string_view publisherFixture =
        "RIFELOVE MODEL CATALOG 1\n"
        "key-id=rifelove-model-catalog-2026-v1\n"
        "payload=eyJzY2hlbWFWZXJzaW9uIjoxLCJtb2RlbHMiOltdfQ==\n"
        "signature=Mwd9cRdrXnlo3ktiH1Yl9TRlMlw3bSWznggHsgO0fkHjGdIuQi28taUWgRsIYq/OeyegFbeXl1+F/KLDT6VZtw==\n";
    bool publisherKeyAccepted = false;
    try {
        publisherKeyAccepted = verifyOfficialNativeModelCatalog(publisherFixture) ==
            R"({"schemaVersion":1,"models":[]})";
    } catch (...) { }
    check(publisherKeyAccepted, "verify a publisher-signed fixture with the application's compiled trust root");

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) {
        check(false, "open P-256 for signed model catalog test");
        return;
    }
    const auto close = [&] {
        if (key) BCryptDestroyKey(key);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    bool generated = BCryptGenerateKeyPair(algorithm, &key, 256, 0) >= 0 &&
        BCryptFinalizeKeyPair(key, 0) >= 0;
    std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + 64> publicBlob{};
    ULONG publicBytes = static_cast<ULONG>(publicBlob.size());
    if (generated) generated = BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, publicBlob.data(),
        publicBytes, &publicBytes, 0) >= 0 && publicBytes == publicBlob.size();
    if (!generated) {
        close();
        check(false, "generate a P-256 catalog fixture key");
        return;
    }
    const auto* publicHeader = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(publicBlob.data());
    check(publicHeader->dwMagic == BCRYPT_ECDSA_PUBLIC_P256_MAGIC && publicHeader->cbKey == 32,
        "export the expected P-256 catalog fixture key");
    NativeModelCatalogTrust trust;
    trust.keyId = "test-catalog-key-v1";
    std::memcpy(trust.publicPoint.data(), publicBlob.data() + sizeof(BCRYPT_ECCKEY_BLOB), trust.publicPoint.size());

    const std::string payload = R"({"schemaVersion":1,"models":[]})";
    const auto digest = nativeCatalogTestDigest(payload);
    std::array<UCHAR, 64> signature{};
    ULONG signatureBytes = 0;
    const bool signedPayload = BCryptSignHash(key, nullptr, const_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()), signature.data(), static_cast<ULONG>(signature.size()), &signatureBytes, 0) >= 0 &&
        signatureBytes == signature.size();
    close();
    check(signedPayload, "sign a catalog fixture using P-256 and SHA-256");
    if (!signedPayload) return;

    const auto makeEnvelope = [&](std::string_view keyId, std::string_view signedPayloadBytes,
                                  const std::array<UCHAR, 64>& signatureBytesValue, bool appendExtraLine = false) {
        std::string result = "RIFELOVE MODEL CATALOG 1\nkey-id=";
        result.append(keyId);
        result += "\npayload=";
        result += nativeCatalogTestBase64(reinterpret_cast<const UCHAR*>(signedPayloadBytes.data()), signedPayloadBytes.size());
        result += "\nsignature=";
        result += nativeCatalogTestBase64(signatureBytesValue.data(), signatureBytesValue.size());
        result += "\n";
        if (appendExtraLine) result += "unexpected\n";
        return result;
    };
    const auto envelope = makeEnvelope(trust.keyId, payload, signature);
    bool validAccepted = false;
    try { validAccepted = verifyNativeSignedModelCatalog(envelope, trust) == payload; }
    catch (...) { }
    check(validAccepted, "accept an authentic signed catalog and return its exact payload bytes");

    auto crlfEnvelope = envelope;
    for (std::size_t offset = 0; offset < crlfEnvelope.size(); ++offset) {
        if (crlfEnvelope[offset] == '\n') { crlfEnvelope.insert(offset, 1, '\r'); ++offset; }
    }
    bool crlfAccepted = false;
    try { crlfAccepted = verifyNativeSignedModelCatalog(crlfEnvelope, trust) == payload; }
    catch (...) { }
    check(crlfAccepted, "accept an authentic catalog envelope with Windows line endings");

    auto alteredPayload = payload;
    alteredPayload[2] = 'x';
    bool payloadTamperingRejected = false;
    try { (void)verifyNativeSignedModelCatalog(makeEnvelope(trust.keyId, alteredPayload, signature), trust); }
    catch (const std::exception&) { payloadTamperingRejected = true; }
    check(payloadTamperingRejected, "reject a catalog payload changed after signing");

    auto badSignature = signature;
    badSignature[0] ^= 1;
    bool signatureTamperingRejected = false;
    try { (void)verifyNativeSignedModelCatalog(makeEnvelope(trust.keyId, payload, badSignature), trust); }
    catch (const std::exception&) { signatureTamperingRejected = true; }
    check(signatureTamperingRejected, "reject a catalog signature changed after signing");

    bool keyRotationRejected = false;
    try { (void)verifyNativeSignedModelCatalog(makeEnvelope("unknown-key", payload, signature), trust); }
    catch (const std::exception&) { keyRotationRejected = true; }
    check(keyRotationRejected, "reject an untrusted catalog key ID");

    bool extraFieldRejected = false;
    try { (void)verifyNativeSignedModelCatalog(makeEnvelope(trust.keyId, payload, signature, true), trust); }
    catch (const std::exception&) { extraFieldRejected = true; }
    check(extraFieldRejected, "reject extra fields in the signed catalog envelope");
}

} // namespace rifelove::native_test

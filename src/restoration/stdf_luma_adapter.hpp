// Independent development adapter. Deliberately not registered by the app.
#pragma once

#include "restoration/video_restoration_backend.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rifelove::validation::stdf {

inline constexpr const char* graphSha256 = "dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721";
inline constexpr const char* adapterId = "stdf-r3-y7-luma-v1";
inline constexpr int adapterVersion = 1;
inline constexpr const char* numericalProfile = "sdr8-fp32-stdf-r3-cpu-reference";
inline constexpr const char* cpuValidationBackend = "onnxruntime_cpu_stdf_r3";
inline constexpr const char* cudaValidationBackend = "onnxruntime_cuda_stdf_r3";
inline constexpr int qualifiedWidth = 384, qualifiedHeight = 256;
// Profile completeness counts are tied to graphSha256 and the pinned ORT
// 1.22.0 CUDA session options. Exact counts reject both a truncated profile
// and an unexpected graph/provider assignment that happens to exceed a floor.
inline constexpr std::size_t qualifiedCudaConvKernelCount = 20;
inline constexpr std::size_t qualifiedCudaGridSampleKernelCount = 9;
enum class Failure { cancelled, invalidRequest, integrity, memoryBudget, nonFiniteOutput };
class Error final : public std::runtime_error {
public:
    Error(Failure failure, const std::string& message) : std::runtime_error(message), kind(failure) {}
    Failure kind;
};

inline void stopped(std::stop_token stop) {
    if (stop.stop_requested()) throw Error(Failure::cancelled, "STDF request cancelled; result discarded");
}
inline std::size_t multiply(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw Error(Failure::memoryBudget, "STDF size multiplication overflow");
    return a * b;
}
inline std::size_t add(std::size_t a, std::size_t b) {
    if (a > std::numeric_limits<std::size_t>::max() - b)
        throw Error(Failure::memoryBudget, "STDF size addition overflow");
    return a + b;
}

inline std::string sha256(std::span<const std::byte> bytes) {
    struct Handles {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        std::vector<unsigned char> object;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } handles;
    auto check = [](NTSTATUS status) { if (status < 0) throw Error(Failure::integrity, "Windows SHA-256 failed"); };
    check(BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    DWORD objectSize = 0, written = 0;
    check(BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0));
    handles.object.resize(objectSize);
    check(BCryptCreateHash(handles.algorithm, &handles.hash, handles.object.data(), objectSize, nullptr, 0, 0));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        check(BCryptHashData(handles.hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + offset)), count, 0));
        offset += count;
    }
    std::array<unsigned char, 32> digest{};
    check(BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    // The hash object must be released before its backing object vector dies.
    BCryptDestroyHash(handles.hash); handles.hash = nullptr;
    std::ostringstream text;
    for (auto byte : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return text.str();
}

class VerifiedWeights final {
public:
    static VerifiedWeights load(const std::filesystem::path& path, std::size_t maximumBytes = 2 * 1024 * 1024) {
        if (!std::filesystem::is_regular_file(path)) throw Error(Failure::integrity, "Local STDF graph must be a regular file");
        const auto count = std::filesystem::file_size(path);
        if (count == 0 || count > maximumBytes || count > std::numeric_limits<std::size_t>::max())
            throw Error(Failure::memoryBudget, "STDF graph exceeds the verified graph byte limit");
        std::vector<std::byte> bytes(static_cast<std::size_t>(count));
        std::ifstream stream(path, std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || stream.peek() != EOF)
            throw Error(Failure::integrity, "STDF graph read changed size or failed");
        if (sha256(bytes) != graphSha256) throw Error(Failure::integrity, "STDF graph does not match the independently validated weight identity");
        return VerifiedWeights(std::move(bytes));
    }
    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }
private:
    explicit VerifiedWeights(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
    std::vector<std::byte> bytes_;
};

struct Limits {
    // Limits apply to adapter payloads, not ORT activations, allocator overhead,
    // decoder storage beyond the exposed plane views, or total device/process RAM.
    // Callers may lower these ceilings, but cannot widen qualification.
    int maximumWidth = qualifiedWidth;
    int maximumHeight = qualifiedHeight;
    std::size_t maximumOwnedPayloadBytes = 16 * 1024 * 1024;
    std::size_t maximumRetainedPlaneSpanBytes = 16 * 1024 * 1024;
};
struct SceneBounds {
    std::size_t firstIndex = 0;
    std::optional<std::size_t> lastIndex; // Inclusive; required for missing future context.
    std::uint64_t serial = 0;
};
// The caller supplies buffer extents from trusted decoder-owned storage. A raw
// DecodedFrame pointer/stride alone cannot prove how many bytes are accessible.
using BoundedPlanes = std::array<std::span<const std::uint8_t>, 3>;
class Plan final {
public:
    [[nodiscard]] std::array<int, 4> geometry() const { return {width, height, paddedWidth, paddedHeight}; }
    [[nodiscard]] const std::array<std::int64_t, 4>& shape() const { return inputShape; }
    [[nodiscard]] const std::array<std::size_t, 7>& slots() const { return contextIndices; }
    [[nodiscard]] std::size_t outputCount() const { return paddedPixels; }
    [[nodiscard]] std::size_t ownedBytes() const { return ownedPayloadBytes; }
    [[nodiscard]] std::size_t retainedBytes() const { return retainedPlaneSpanBytes; }
    [[nodiscard]] const media::DecodedFrame& center() const { return context.at(centerIndex); }
    [[nodiscard]] std::stop_token stopToken() const { return stop; }
private:
    Plan() = default;
    friend Plan plan(const restoration::Request&, std::span<const BoundedPlanes>, const SceneBounds&, const VerifiedWeights&, Limits);
    friend std::vector<float> pack(const Plan&);
    friend restoration::Result complete(const Plan&, std::span<const float>, float, double);
    int width = 0, height = 0, paddedWidth = 0, paddedHeight = 0;
    std::array<std::int64_t, 4> inputShape{};
    std::size_t paddedPixels = 0, inputFloats = 0, ownedPayloadBytes = 0, retainedPlaneSpanBytes = 0;
    std::array<std::size_t, 7> contextIndices{};
    // Freeze the validated metadata and retain owners. The caller can replace
    // its Request after planning without invalidating this plan's plane views.
    std::vector<media::DecodedFrame> context;
    std::size_t centerIndex = 0;
    std::stop_token stop;
};

inline Plan plan(const restoration::Request& request, std::span<const BoundedPlanes> extents,
                 const SceneBounds& scene, const VerifiedWeights& model, Limits limits = {}) {
    stopped(request.stop);
    auto invalid = [](const char* message) { throw Error(Failure::invalidRequest, message); };
    if (request.task != restoration::Task::deblock || request.placement != restoration::Placement::sourceRateBeforeInterpolation ||
        request.inputProfile != "bt709-sdr8") invalid("STDF needs source-rate deblock and explicit bt709-sdr8");
    if (request.missingFramesBefore > 3 || request.missingFramesAfter > 3 ||
        request.context.size() != 7 - request.missingFramesBefore - request.missingFramesAfter ||
        request.centerIndex != 3 - request.missingFramesBefore || extents.size() != request.context.size())
        invalid("STDF requires seven declared real/edge-padded temporal slots");
    if (scene.lastIndex && *scene.lastIndex < scene.firstIndex) invalid("STDF scene bounds are inverted");
    const auto& center = request.context.at(request.centerIndex);
    if (center.width <= 0 || center.height <= 0 || limits.maximumWidth <= 0 || limits.maximumHeight <= 0 ||
        center.width > limits.maximumWidth || center.height > limits.maximumHeight ||
        center.width > qualifiedWidth || center.height > qualifiedHeight ||
        center.width > std::numeric_limits<int>::max() - 7 || center.height > std::numeric_limits<int>::max() - 7)
        throw Error(Failure::memoryBudget, "STDF dimensions exceed the independently qualified adapter limit");
    Plan result;
    result.width = center.width; result.height = center.height;
    result.paddedWidth = (center.width + 7) / 8 * 8; result.paddedHeight = (center.height + 7) / 8 * 8;
    result.paddedPixels = multiply(static_cast<std::size_t>(result.paddedWidth), result.paddedHeight);
    result.inputFloats = multiply(7, result.paddedPixels);
    result.inputShape = {1, 7, result.paddedHeight, result.paddedWidth};
    result.ownedPayloadBytes = add(model.bytes().size(), add(multiply(add(result.inputFloats, result.paddedPixels), sizeof(float)),
                                                          multiply(center.width, center.height)));
    if (result.ownedPayloadBytes > limits.maximumOwnedPayloadBytes)
        throw Error(Failure::memoryBudget, "STDF input/output/graph payload plan exceeds the host-memory budget");
    for (std::size_t index = 0; index < request.context.size(); ++index) {
        const auto& frame = request.context[index];
        const auto& color = frame.color;
        if (!frame.storage || !frame.timestamp || frame.width != center.width || frame.height != center.height ||
            color.pixelFormat != "yuv420p" || color.bitDepth != 8 || color.primaries != "bt709" ||
            color.transfer != "bt709" || color.matrix != "bt709" || color.range != "tv" || color.hdrTransfer ||
            color.masteringDisplay || color.contentLight)
            invalid("STDF initial adapter requires owned planar BT.709 limited-range 8-bit SDR with exact timestamps");
        if (frame.duration && *frame.duration <= Rational(0)) invalid("STDF frame duration must be positive when known");
        if (frame.index < scene.firstIndex || (scene.lastIndex && frame.index > *scene.lastIndex))
            invalid("STDF temporal context crosses the declared scene boundary");
        if (index) {
            const auto& previous = request.context[index - 1];
            if (!(*previous.timestamp < *frame.timestamp) || previous.index == std::numeric_limits<std::size_t>::max() ||
                frame.index != previous.index + 1) invalid("STDF real context must have adjacent indices and strictly increasing exact PTS");
        }
        const std::array<int, 3> widths{frame.width, (frame.width + 1) / 2, (frame.width + 1) / 2};
        const std::array<int, 3> heights{frame.height, (frame.height + 1) / 2, (frame.height + 1) / 2};
        for (std::size_t plane = 0; plane < 3; ++plane) {
            const auto& view = frame.planes[plane];
            if (!view.data || view.stride < widths[plane] || extents[index][plane].data() != view.data)
                invalid("STDF needs positive valid plane strides and matching bounded decoder views");
            const auto spanBytes = add(multiply(static_cast<std::size_t>(view.stride), heights[plane] - 1), widths[plane]);
            if (extents[index][plane].size() < spanBytes) invalid("STDF plane buffer is shorter than its geometry");
            result.retainedPlaneSpanBytes = add(result.retainedPlaneSpanBytes, spanBytes);
        }
    }
    if (result.retainedPlaneSpanBytes > limits.maximumRetainedPlaneSpanBytes)
        throw Error(Failure::memoryBudget, "STDF retained decoded plane spans exceed the explicit span budget");
    if (request.missingFramesBefore && request.context.front().index != scene.firstIndex)
        invalid("STDF cannot invent missing history inside a scene");
    if (request.missingFramesAfter && (!scene.lastIndex || request.context.back().index != *scene.lastIndex))
        invalid("STDF cannot invent missing future frames before a verified scene/clip end");
    for (int slot = 0; slot < 7; ++slot)
        result.contextIndices[slot] = static_cast<std::size_t>(std::clamp(slot - static_cast<int>(request.missingFramesBefore),
                                                                       0, static_cast<int>(request.context.size()) - 1));
    result.context = request.context; result.centerIndex = request.centerIndex; result.stop = request.stop;
    return result;
}

inline std::vector<float> pack(const Plan& layout) {
    stopped(layout.stop);
    std::vector<float> tensor(layout.inputFloats);
    for (std::size_t slot = 0; slot < 7; ++slot) {
        const auto& frame = layout.context.at(layout.contextIndices[slot]);
        for (int row = 0; row < layout.paddedHeight; ++row) {
            stopped(layout.stop);
            const auto* source = frame.planes[0].data + static_cast<std::size_t>(std::min(row, layout.height - 1)) * frame.planes[0].stride;
            auto* destination = tensor.data() + slot * layout.paddedPixels + static_cast<std::size_t>(row) * layout.paddedWidth;
            for (int column = 0; column < layout.paddedWidth; ++column)
                destination[column] = static_cast<float>(source[std::min(column, layout.width - 1)]) / 255.0F;
        }
    }
    stopped(layout.stop);
    return tensor;
}

struct RetainedOutput {
    std::vector<std::uint8_t> luma;
    std::shared_ptr<const void> originalCenter;
};

inline restoration::Result complete(const Plan& layout, std::span<const float> prediction,
                                    float strength, double elapsedSeconds = 0) {
    stopped(layout.stop); // ORT can return success after an in-flight cancel.
    if (!std::isfinite(strength) || strength < 0 || strength > 1 || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0)
        throw Error(Failure::invalidRequest, "STDF explicit strength/time is outside its supported interval");
    if (prediction.size() != layout.paddedPixels)
        throw Error(Failure::invalidRequest, "STDF output tensor does not match its planned shape");
    if (!std::all_of(prediction.begin(), prediction.end(), [](float pixel) { return std::isfinite(pixel); }))
        throw Error(Failure::nonFiniteOutput, "STDF returned a non-finite Y code value");
    const auto& center = layout.context.at(layout.centerIndex);
    auto storage = std::make_shared<RetainedOutput>();
    storage->luma.resize(multiply(layout.width, layout.height));
    storage->originalCenter = center.storage;
    for (int row = 0; row < layout.height; ++row) {
        stopped(layout.stop);
        for (int column = 0; column < layout.width; ++column) {
            const float original = center.planes[0].data[static_cast<std::size_t>(row) * center.planes[0].stride + column];
            const float restored = std::clamp(prediction[static_cast<std::size_t>(row) * layout.paddedWidth + column], 0.0F, 1.0F) * 255.0F;
            storage->luma[static_cast<std::size_t>(row) * layout.width + column] =
                static_cast<std::uint8_t>(std::lround(std::clamp(original + strength * (restored - original), 0.0F, 255.0F)));
        }
    }
    auto output = center; // Exact index/PTS/duration/color/recovery metadata.
    output.storage = storage;
    output.planes[0] = {storage->luma.data(), layout.width,
                        media::FramePlaneExtent{storage->luma.data(), storage->luma.size(), 0}};
    stopped(layout.stop);
    return {std::move(output), elapsedSeconds}; // U/V pointers/strides stay unchanged.
}

template<class Inference>
restoration::Result run(const VerifiedWeights& model, const restoration::Request& request,
                        std::span<const BoundedPlanes> extents, const SceneBounds& scene,
                        float strength, Inference&& inference, Limits limits = {}) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw Error(Failure::invalidRequest, "STDF strength must be finite in [0,1]");
    const auto layout = plan(request, extents, scene, model, limits);
    if (strength == 0) { stopped(layout.stopToken()); return {layout.center(), 0}; }
    auto input = pack(layout);
    // An incomplete callback must fail the finite-output check, never look like
    // a successfully restored all-black/partially zero-filled image.
    std::vector<float> output(layout.outputCount(), std::numeric_limits<float>::quiet_NaN());
    const auto begin = std::chrono::steady_clock::now();
    stopped(layout.stopToken());
    std::invoke(std::forward<Inference>(inference), model.bytes(), std::span<const float>(input), layout.shape(),
                std::span<float>(output), layout.stopToken());
    stopped(layout.stopToken());
    return complete(layout, output, strength,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
}

} // namespace rifelove::validation::stdf

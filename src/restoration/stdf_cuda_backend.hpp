#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#undef _Check_return_
#undef _In_reads_
#undef _Inout_updates_
#undef _Out_writes_
#undef _Inout_updates_all_
#undef _Out_writes_bytes_all_
#undef _Out_writes_all_
#undef _Success_
#undef _Outptr_result_buffer_maybenull_
#include <onnxruntime_c_api.h>
#endif
#include "inference/ort_runtime_lease.hpp"
#include "restoration/stdf_luma_adapter.hpp"

#include <filesystem>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace rifelove::restoration {

struct ModelPack;

struct StdfCudaConfig {
    std::filesystem::path runtimeDirectory;
    std::filesystem::path profilePrefix;
    std::size_t arenaBudgetBytes = 1024ULL * 1024 * 1024;
    std::size_t hostFloatBufferLimit = 512ULL * 1024 * 1024;
    std::size_t maxQueuedRequests = 1;
    int deviceOrdinal = 0;
    float strength = 1.0F;
    // The current graph has only been qualified through the adapter's
    // 384x256 ceiling. Larger common video sizes remain deliberately blocked
    // until a separately verified tile/full-frame path exists.
    int maximumWidth = validation::stdf::qualifiedWidth;
    int maximumHeight = validation::stdf::qualifiedHeight;
    bool requireCriticalCudaProfile = true;
};

struct StdfCudaDiagnostics {
    std::string runtimeVersion;
    std::string backend = "onnxruntime_cuda";
    bool cudaProviderAvailable = false;
    bool cudaSessionCreated = false;
    bool criticalComputeVerified = false;
    bool cpuImageFallbackDetected = false;
    std::size_t cudaNodeCount = 0;
    std::size_t cudaConvNodeCount = 0;
    std::size_t cudaCriticalNodeCount = 0;
    std::size_t cudaGridSampleNodeCount = 0;
    std::size_t cpuControlNodeCount = 0;
    std::size_t unknownProviderKernelCount = 0;
    std::string fallbackReason;
    std::filesystem::path profilePath;
};

// Small, injectable contracts shared by the production ORT path and its
// boundary tests. They intentionally carry only inspected metadata/state;
// callers still own the OrtValue and session lifetime.
struct StdfCudaProfileGateState {
    bool verified = false;
    bool failed = false;
};

[[nodiscard]] bool acceptStdfCudaProfile(
    StdfCudaProfileGateState& state,
    const StdfCudaDiagnostics& diagnostics,
    bool requireCriticalCudaProfile,
    std::string& rejectionReason);

struct StdfCudaOutputTensorMetadata {
    bool isTensor = false;
    bool isFp32 = false;
    std::size_t rank = 0;
    std::array<std::int64_t, 4> shape{};
    std::size_t elementCount = 0;
};

void validateStdfCudaOutputTensorContract(
    const StdfCudaOutputTensorMetadata& metadata,
    std::int64_t expectedHeight,
    std::int64_t expectedWidth,
    std::size_t expectedElementCount);

// Developer-facing profile policy hook. It is also used by the backend before
// publishing the first result; malformed or incomplete kernel assignment is a
// rejection, never an implicit success.
[[nodiscard]] StdfCudaDiagnostics inspectStdfCudaProfile(const std::filesystem::path& path);

// Experimental source-rate STDF-R3 runner. It is intentionally separate from
// the generic restoration backend until decoder plane extents and scene
// scheduling are wired by the owning contracts layer.
class StdfCudaBackend final {
public:
    explicit StdfCudaBackend(StdfCudaConfig config);
    ~StdfCudaBackend();
    StdfCudaBackend(const StdfCudaBackend&) = delete;
    StdfCudaBackend& operator=(const StdfCudaBackend&) = delete;

    void loadModel(const std::filesystem::path& graphPath);
    [[nodiscard]] media::DecodedFrame restore(
        const Request& request,
        std::span<const validation::stdf::BoundedPlanes> extents,
        const validation::stdf::SceneBounds& scene,
        float strength = 1.0F);
    [[nodiscard]] const StdfCudaDiagnostics& diagnostics() const noexcept;
    [[nodiscard]] std::filesystem::path finishProfiling();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Serialized IVideRestorationBackend adapter for the reviewed STDF-R3 graph.
// It is intentionally opt-in: callers must load a strictly qualified
// restoration::ModelPack before any request can be queued. The worker owns a
// single low-level runner and keeps all seven-frame plane spans alive through
// the future, so ORT sessions are never used concurrently or after teardown.
class StdfCudaVideoRestorationBackend final : public IVideoRestorationBackend {
public:
    explicit StdfCudaVideoRestorationBackend(StdfCudaConfig config);
    ~StdfCudaVideoRestorationBackend() override;
    StdfCudaVideoRestorationBackend(const StdfCudaVideoRestorationBackend&) = delete;
    StdfCudaVideoRestorationBackend& operator=(const StdfCudaVideoRestorationBackend&) = delete;

    [[nodiscard]] Capabilities capabilities() const override;
    void loadModel(const ModelPack& model);
    [[nodiscard]] std::future<Result> restore(Request request) override;
    [[nodiscard]] std::filesystem::path finishProfiling();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rifelove::restoration

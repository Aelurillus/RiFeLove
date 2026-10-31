#include "restoration/stdf_cuda_backend.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/stdf_tiled_adapter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#include <utility>

namespace rifelove::restoration {
namespace {

using Json = nlohmann::json;

class StdfError final : public std::runtime_error {
public:
    explicit StdfError(const std::string& message) : std::runtime_error(message) {}
};

template<class T> struct Handle final {
    T* value{};
    void(ORT_API_CALL* release)(T*){};
    Handle() = default;
    explicit Handle(void(ORT_API_CALL* deleter)(T*)) : release(deleter) {}
    ~Handle() { if (value && release) release(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)), release(other.release) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (value && release) release(value); value = std::exchange(other.value, nullptr); release = other.release; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

void throwStatus(const OrtApi& api, OrtStatus* status, std::stop_token stop = {}) {
    if (!status) return;
    const std::string message = api.GetErrorMessage(status);
    const auto code = api.GetErrorCode(status);
    api.ReleaseStatus(status);
    if (stop.stop_requested()) throw StdfError("STDF CUDA inference cancelled; result discarded");
    if (code == ORT_NOT_IMPLEMENTED ||
        (message.find("CUDA") != std::string::npos && message.find("provider") != std::string::npos))
        throw StdfError("STDF CUDA provider/runtime error: " + message);
    if (message.find("out of memory") != std::string::npos || message.find("Failed to allocate memory") != std::string::npos)
        throw StdfError("STDF CUDA memory budget/error: " + message);
    throw StdfError("STDF ONNX Runtime error: " + message);
}

void check(const OrtApi& api, OrtStatus* status, std::stop_token stop = {}) { throwStatus(api, status, stop); }

std::filesystem::path profilePath(const StdfCudaConfig& config) {
    if (!config.profilePrefix.empty()) return config.profilePrefix;
    auto path = std::filesystem::temp_directory_path() / "RifeLove" / "stdf-cuda-profile";
    std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
    if (error) throw StdfError("Could not create STDF CUDA profile directory: " + error.message());
    const auto nonce = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return path.parent_path() / (path.filename().string() + "-" + std::to_string(nonce));
}

void cleanupTemporaryProfile(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    if (path.empty() || prefix.empty()) return;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return;
    error.clear();
    const auto actualParent = std::filesystem::weakly_canonical(path.parent_path(), error);
    if (error) return;
    error.clear();
    const auto prefixParent = std::filesystem::weakly_canonical(prefix.parent_path(), error);
    if (error || actualParent != prefixParent) return;
    const auto actualName = path.filename().string();
    const auto prefixName = prefix.filename().string();
    if (prefixName.empty() || actualName.rfind(prefixName, 0) != 0) return;
    std::filesystem::remove(path, error);
}

std::vector<validation::stdf::BoundedPlanes> provePlaneSpans(const Request& request) {
    std::vector<validation::stdf::BoundedPlanes> result;
    result.reserve(request.context.size());
    for (std::size_t index = 0; index < request.context.size(); ++index) {
        validation::stdf::BoundedPlanes planes{};
        const auto& frame = request.context[index];
        for (std::size_t plane = 0; plane < planes.size(); ++plane) {
            const auto span = media::provenPlaneSpan(frame.planes[plane]);
            if (!span || span->empty())
                throw StdfError("STDF request requires a proven non-empty decoder plane span for every Y/U/V plane");
            planes[plane] = *span;
        }
        result.push_back(planes);
    }
    return result;
}

Capabilities stdfCapabilities(const ModelPack& model, const StdfCudaDiagnostics& diagnostics,
                               const StdfCudaConfig& config) {
    auto capabilities = model.capabilities;
    capabilities.runtimeBackend = diagnostics.cudaProviderAvailable && diagnostics.cudaSessionCreated
        ? diagnostics.backend : "unavailable";
    capabilities.fallbackReason = diagnostics.fallbackReason;
    if (capabilities.fallbackReason.empty() && !diagnostics.criticalComputeVerified)
        capabilities.fallbackReason = "CUDA profile verification is required before the first result is publishable";
    capabilities.runtimeSignature = "stdf-r3-y7-luma-v1;graph-" + model.graphSha256 +
        ";ort-" + diagnostics.runtimeVersion + ";provider-onnxruntime_cuda" +
        ";max-" + std::to_string(config.maximumWidth) + "x" + std::to_string(config.maximumHeight) +
        ";strategy-center-uv-pts-v1;profile-required-before-publish-v1;serialized-worker-v1";
    if (config.enableTiling)
        capabilities.runtimeSignature += std::string(";tilePolicy-") + stdfTilePolicy(config) +
            ";source-axis-3840;source-pixels-8294400;evaluation-not-full-frame-equivalence-v1";
    return capabilities;
}

bool allowedCpuOp(const std::string& op) {
    static const std::set<std::string> allowed{"Gather", "Unsqueeze", "Mul", "Concat", "Cast", "Div"};
    return allowed.contains(op);
}

bool kernelEvent(const Json& event) {
    const auto name = event.value("name", "");
    return !name.empty() && name.ends_with("_kernel_time");
}

bool criticalImageOp(const std::string& op) {
    static const std::set<std::string> critical{"Conv", "ConvTranspose", "GridSample", "Gemm", "MatMul"};
    return critical.contains(op);
}

struct ProfileResult { StdfCudaDiagnostics diagnostics; };

ProfileResult inspectProfile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw StdfError("Could not open ONNX Runtime CUDA profile: " + path.string());
    Json events; try { stream >> events; } catch (const std::exception& e) { throw StdfError(std::string("Invalid ONNX Runtime profile: ") + e.what()); }
    ProfileResult result;
    auto& d = result.diagnostics;
    for (const auto& event : events) {
        if (!event.is_object() || event.value("cat", "") != "Node" || !kernelEvent(event)) continue;
        const auto args = event.value("args", Json::object());
        if (!args.is_object() || !args.contains("provider") || !args.contains("op_name")) {
            ++d.unknownProviderKernelCount; d.cpuImageFallbackDetected = true; continue;
        }
        if (!args.at("provider").is_string() || !args.at("op_name").is_string()) {
            ++d.unknownProviderKernelCount; d.cpuImageFallbackDetected = true; continue;
        }
        const auto provider = args.at("provider").get<std::string>();
        const auto op = args.at("op_name").get<std::string>();
        if (provider == "CUDAExecutionProvider") {
            ++d.cudaNodeCount;
            if (op == "Conv") ++d.cudaConvNodeCount;
            if (criticalImageOp(op)) ++d.cudaCriticalNodeCount;
            if (op == "GridSample") ++d.cudaGridSampleNodeCount;
            continue;
        }
        if (provider != "CPUExecutionProvider") { ++d.unknownProviderKernelCount; d.cpuImageFallbackDetected = true; continue; }
        bool scalarOnly = allowedCpuOp(op) && args.contains("output_type_shape") && args.at("output_type_shape").is_array() && !args.at("output_type_shape").empty();
        if (scalarOnly) for (const auto& typed : args.at("output_type_shape")) {
            if (!typed.is_object() || typed.empty()) { scalarOnly = false; break; }
            for (auto it = typed.begin(); it != typed.end(); ++it) {
                const auto& shape = it.value();
                std::size_t elements = 1;
                if (!shape.is_array()) { scalarOnly = false; break; }
                for (const auto& dimension : shape) {
                    if (!dimension.is_number_integer() || dimension.get<std::int64_t>() < 0) { scalarOnly = false; break; }
                    const auto size = dimension.get<std::int64_t>();
                    if (size > 8 || elements > 8 / std::max<std::size_t>(1, static_cast<std::size_t>(size))) scalarOnly = false;
                    elements *= static_cast<std::size_t>(size);
                }
                if (it.key() != "int64" && !shape.empty()) scalarOnly = false;
            }
        }
        if (scalarOnly) ++d.cpuControlNodeCount; else d.cpuImageFallbackDetected = true;
    }
    d.criticalComputeVerified = d.cudaConvNodeCount == validation::stdf::qualifiedCudaConvKernelCount &&
                                d.cudaGridSampleNodeCount == validation::stdf::qualifiedCudaGridSampleKernelCount &&
                                !d.cpuImageFallbackDetected && d.unknownProviderKernelCount == 0;
    if (!d.criticalComputeVerified) {
        if (d.cpuImageFallbackDetected) d.fallbackReason = "CPUExecutionProvider or unknown provider executed unqualified image work";
        else if (d.cudaConvNodeCount != validation::stdf::qualifiedCudaConvKernelCount ||
                 d.cudaGridSampleNodeCount != validation::stdf::qualifiedCudaGridSampleKernelCount)
            d.fallbackReason = "Profile does not contain the exact qualified CUDA Conv/GridSample kernel evidence";
        else d.fallbackReason = "Profile provider assignment is incomplete";
    }
    d.profilePath = path;
    return result;
}

} // namespace

StdfCudaDiagnostics inspectStdfCudaProfile(const std::filesystem::path& path) {
    return inspectProfile(path).diagnostics;
}

bool acceptStdfCudaProfile(StdfCudaProfileGateState& state,
                           const StdfCudaDiagnostics& diagnostics,
                           bool requireCriticalCudaProfile,
                           std::string& rejectionReason) {
    if (state.failed) {
        rejectionReason = "STDF CUDA profile verification already failed for this session";
        return false;
    }
    if (requireCriticalCudaProfile && !diagnostics.criticalComputeVerified) {
        state.failed = true;
        rejectionReason = "STDF CUDA profile rejected the provider assignment: " + diagnostics.fallbackReason;
        return false;
    }
    state.verified = true;
    rejectionReason.clear();
    return true;
}

void validateStdfCudaOutputTensorContract(const StdfCudaOutputTensorMetadata& metadata,
                                          std::int64_t expectedHeight,
                                          std::int64_t expectedWidth,
                                          std::size_t expectedElementCount) {
    if (!metadata.isTensor) throw StdfError("STDF CUDA graph returned a non-tensor output");
    if (!metadata.isFp32 || metadata.rank != 4)
        throw StdfError("STDF CUDA output must be an FP32 rank-four tensor");
    const std::array<std::int64_t, 4> expectedShape{1, 1, expectedHeight, expectedWidth};
    if (metadata.shape != expectedShape)
        throw StdfError("STDF CUDA output tensor shape contract mismatch");
    if (metadata.elementCount != expectedElementCount)
        throw StdfError("STDF CUDA output tensor element count contract mismatch");
}

class StdfCudaBackend::Impl final {
public:
    explicit Impl(StdfCudaConfig config) : config_(std::move(config)), diagnostics_{} {
        if (config_.arenaBudgetBytes == 0 || config_.hostFloatBufferLimit == 0 || config_.deviceOrdinal < 0)
            throw StdfError("STDF CUDA resource configuration is invalid");
        if (config_.maximumWidth <= 0 || config_.maximumHeight <= 0 ||
            config_.maximumWidth > validation::stdf::qualifiedWidth || config_.maximumHeight > validation::stdf::qualifiedHeight)
            throw StdfError("STDF CUDA shape exceeds the currently qualified 384x256 ceiling");
        lease_ = inference::acquireOrtRuntimeLease(config_.runtimeDirectory);
        diagnostics_.runtimeVersion = "1.22.0";
        diagnostics_.cudaProviderAvailable = lease_.cudaProviderAvailable;
        diagnostics_.tilingEnabled = config_.enableTiling;
        diagnostics_.tilePolicy = stdfTilePolicy(config_);
    }
    void load(const std::filesystem::path& graphPath) {
        model_.emplace(validation::stdf::VerifiedWeights::load(graphPath));
        const auto& api = *lease_.api;
        Handle<OrtSessionOptions> options(api.ReleaseSessionOptions); check(api, api.CreateSessionOptions(&options.value));
        check(api, api.SetSessionExecutionMode(options.value, ORT_SEQUENTIAL));
        check(api, api.SetIntraOpNumThreads(options.value, 2)); check(api, api.SetInterOpNumThreads(options.value, 1));
        check(api, api.SetSessionGraphOptimizationLevel(options.value, ORT_ENABLE_EXTENDED));
        profilePrefix_ = profilePath(config_);
        check(api, api.EnableProfiling(options.value, profilePrefix_.c_str())); profiling_ = true;
        Handle<OrtCUDAProviderOptionsV2> cuda(api.ReleaseCUDAProviderOptions); check(api, api.CreateCUDAProviderOptions(&cuda.value));
        const std::string ordinal = std::to_string(config_.deviceOrdinal), budget = std::to_string(config_.arenaBudgetBytes);
        const char* keys[] = {"device_id", "gpu_mem_limit", "arena_extend_strategy", "cudnn_conv_algo_search", "cudnn_conv_use_max_workspace", "use_tf32", "do_copy_in_default_stream"};
        const char* values[] = {ordinal.c_str(), budget.c_str(), "kSameAsRequested", "DEFAULT", "0", "0", "1"};
        check(api, api.UpdateCUDAProviderOptions(cuda.value, keys, values, std::size(keys)));
        check(api, api.SessionOptionsAppendExecutionProvider_CUDA_V2(options.value, cuda.value));
        Handle<OrtSession> session(api.ReleaseSession);
        check(api, api.CreateSessionFromArray(lease_.environment, model_->bytes().data(), model_->bytes().size(), options.value, &session.value));
        inspectContract(api, session.value);
        session_ = std::move(session);
        profiling_ = true;
        profileChecked_ = false;
        profileGate_ = {};
        diagnostics_.cudaSessionCreated = true;
    }
    media::DecodedFrame restore(const Request& request, std::span<const validation::stdf::BoundedPlanes> extents,
                                const validation::stdf::SceneBounds& scene, float strength) {
        if (profileGate_.failed)
            throw StdfError("STDF CUDA session is permanently rejected after profile verification failure");
        if (!session_.value) throw StdfError("Load a verified STDF graph before inference");
        const auto layoutLimits = validation::stdf::Limits{config_.maximumWidth, config_.maximumHeight, config_.hostFloatBufferLimit, config_.hostFloatBufferLimit};
        if (config_.enableTiling) {
            validation::stdf::tiled::Statistics statistics;
            auto remember = [&] {
                diagnostics_.tilingEnabled = true;
                diagnostics_.tilePolicy = stdfTilePolicy(config_);
                diagnostics_.tilesPlanned = statistics.tilesPlanned;
                diagnostics_.tilesCompleted = statistics.tilesCompleted;
                diagnostics_.peakHostFloatBufferBytes = statistics.peakFloatBufferBytes;
                diagnostics_.adapterOwnedPayloadBytes = statistics.ownedPayloadBytes;
                diagnostics_.retainedPlaneSpanBytes = statistics.retainedPlaneSpanBytes;
                diagnostics_.maximumTileInputWidth = statistics.maximumInputWidth;
                diagnostics_.maximumTileInputHeight = statistics.maximumInputHeight;
            };
            try {
                auto result = validation::stdf::tiled::run(*model_, request, extents, scene, strength,
                    [&](std::span<const std::byte> /*graph*/, std::span<const float> input, std::array<std::int64_t,4> shape,
                        std::span<float> output, std::stop_token stop) { runOrt(input, shape, output, stop); },
                    layoutLimits, &statistics);
                remember();
                return std::move(result.centerFrame);
            } catch (...) {
                remember(); // Diagnostic progress is never a published partial frame.
                throw;
            }
        }
        return validation::stdf::run(*model_, request, extents, scene, strength,
            [&](std::span<const std::byte> /*graph*/, std::span<const float> input, std::array<std::int64_t,4> shape,
                std::span<float> output, std::stop_token stop) {
                runOrt(input, shape, output, stop);
            }, layoutLimits).centerFrame;
    }
    const StdfCudaDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    std::filesystem::path finishProfile() {
        if (profileGate_.failed)
            throw StdfError("STDF CUDA profile verification already failed for this session");
        if (!profiling_) return diagnostics_.profilePath;
        std::filesystem::path generatedProfilePath;
        try {
            const auto& api = *lease_.api; OrtAllocator* allocator = nullptr; check(api, api.GetAllocatorWithDefaultOptions(&allocator));
            char* name = nullptr; check(api, api.SessionEndProfiling(session_.value, allocator, &name));
            const auto path = name ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name))) : profilePrefix_;
            generatedProfilePath = path;
            allocator->Free(allocator, name); profiling_ = false;
            const bool providerAvailable = diagnostics_.cudaProviderAvailable;
            const bool sessionCreated = diagnostics_.cudaSessionCreated;
            auto profile = inspectStdfCudaProfile(path);
            profile.runtimeVersion = diagnostics_.runtimeVersion;
            profile.backend = diagnostics_.backend;
            profile.cudaProviderAvailable = providerAvailable;
            profile.cudaSessionCreated = sessionCreated;
            profile.tilingEnabled = diagnostics_.tilingEnabled;
            profile.tilePolicy = diagnostics_.tilePolicy;
            profile.tilesPlanned = diagnostics_.tilesPlanned;
            profile.tilesCompleted = diagnostics_.tilesCompleted;
            profile.peakHostFloatBufferBytes = diagnostics_.peakHostFloatBufferBytes;
            profile.adapterOwnedPayloadBytes = diagnostics_.adapterOwnedPayloadBytes;
            profile.retainedPlaneSpanBytes = diagnostics_.retainedPlaneSpanBytes;
            profile.maximumTileInputWidth = diagnostics_.maximumTileInputWidth;
            profile.maximumTileInputHeight = diagnostics_.maximumTileInputHeight;
            diagnostics_ = std::move(profile);
            std::string rejectionReason;
            if (!acceptStdfCudaProfile(profileGate_, diagnostics_, config_.requireCriticalCudaProfile, rejectionReason))
                throw StdfError(rejectionReason);
            if (config_.profilePrefix.empty()) {
                cleanupTemporaryProfile(generatedProfilePath, profilePrefix_);
                diagnostics_.profilePath.clear();
            }
            profileChecked_ = true;
            return path;
        } catch (...) {
            if (config_.profilePrefix.empty()) cleanupTemporaryProfile(generatedProfilePath, profilePrefix_);
            profiling_ = false;
            profileGate_.failed = true;
            throw;
        }
    }
private:
    void inspectContract(const OrtApi& api, OrtSession* session) {
        std::size_t inputs = 0, outputs = 0; check(api, api.SessionGetInputCount(session, &inputs)); check(api, api.SessionGetOutputCount(session, &outputs));
        if (inputs != 1 || outputs != 1) throw StdfError("STDF graph must have one input and one output");
        OrtAllocator* allocator = nullptr; check(api, api.GetAllocatorWithDefaultOptions(&allocator));
        char* inputName = nullptr; char* outputName = nullptr;
        check(api, api.SessionGetInputName(session, 0, allocator, &inputName)); check(api, api.SessionGetOutputName(session, 0, allocator, &outputName));
        const std::string in = inputName ? inputName : "", out = outputName ? outputName : ""; allocator->Free(allocator, inputName); allocator->Free(allocator, outputName);
        if (in != "luma_frames" || out != "restored_luma") throw StdfError("STDF graph tensor names do not match luma adapter");
        validateTensor(api, session, false, {1, 7, -1, -1}); validateTensor(api, session, true, {-1, 1, -1, -1});
    }
    void validateTensor(const OrtApi& api, OrtSession* session, bool output, std::array<std::int64_t,4> expected) {
        Handle<OrtTypeInfo> type(api.ReleaseTypeInfo); check(api, output ? api.SessionGetOutputTypeInfo(session,0,&type.value) : api.SessionGetInputTypeInfo(session,0,&type.value));
        const OrtTensorTypeAndShapeInfo* tensor = nullptr; check(api, api.CastTypeInfoToTensorInfo(type.value, &tensor));
        ONNXTensorElementDataType dtype{}; std::size_t rank = 0; check(api, api.GetTensorElementType(tensor,&dtype)); check(api, api.GetDimensionsCount(tensor,&rank));
        if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != expected.size()) throw StdfError("STDF graph tensors must be rank-four FP32");
        std::array<std::int64_t,4> actual{}; check(api, api.GetDimensions(tensor, actual.data(), actual.size()));
        for (std::size_t i = 0; i < actual.size(); ++i) if (expected[i] >= 0 && actual[i] != expected[i]) throw StdfError("STDF graph tensor shape contract mismatch");
    }
    void runOrt(std::span<const float> input, std::array<std::int64_t,4> shape, std::span<float> output, std::stop_token stop) {
        const auto& api = *lease_.api; if (stop.stop_requested()) throw StdfError("STDF CUDA inference cancelled; result discarded");
        const auto count = static_cast<std::size_t>(shape[0]) * static_cast<std::size_t>(shape[1]) * static_cast<std::size_t>(shape[2]) * static_cast<std::size_t>(shape[3]);
        if (input.size() != count || output.size() != static_cast<std::size_t>(shape[2]) * static_cast<std::size_t>(shape[3])) throw StdfError("STDF tensor size contract mismatch");
        Handle<OrtMemoryInfo> memory(api.ReleaseMemoryInfo); check(api, api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory.value));
        Handle<OrtValue> inputValue(api.ReleaseValue); check(api, api.CreateTensorWithDataAsOrtValue(memory.value, const_cast<float*>(input.data()), input.size_bytes(), shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue.value));
        const char* inputNames[] = {"luma_frames"}; const char* outputNames[] = {"restored_luma"};
        Handle<OrtValue> outputValue(api.ReleaseValue);
        if (config_.enableTiling) {
            // The tiled adapter proves its sole prediction-buffer budget
            // before allocation. Bind that exact host buffer to ORT rather
            // than retaining a second ORT-allocated host prediction copy.
            const std::array<std::int64_t, 4> outputShape{1, 1, shape[2], shape[3]};
            check(api, api.CreateTensorWithDataAsOrtValue(memory.value, output.data(), output.size_bytes(),
                outputShape.data(), outputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputValue.value));
        }
        Handle<OrtRunOptions> runOptions(api.ReleaseRunOptions); check(api, api.CreateRunOptions(&runOptions.value));
        std::stop_callback terminate(stop, [&] { if (auto* status = api.RunOptionsSetTerminate(runOptions.value)) api.ReleaseStatus(status); });
        check(api, api.Run(session_.value, runOptions.value, inputNames, &inputValue.value, 1, outputNames, 1, &outputValue.value), stop);
        if (stop.stop_requested()) throw StdfError("STDF CUDA inference cancelled; result discarded");
        if (!outputValue.value) throw StdfError("STDF CUDA graph returned no output value");
        enum ONNXType valueType{}; check(api, api.GetValueType(outputValue.value, &valueType));
        Handle<OrtTensorTypeAndShapeInfo> outputInfo(api.ReleaseTensorTypeAndShapeInfo);
        check(api, api.GetTensorTypeAndShape(outputValue.value, &outputInfo.value));
        ONNXTensorElementDataType dtype{}; std::size_t rank = 0;
        check(api, api.GetTensorElementType(outputInfo.value, &dtype));
        check(api, api.GetDimensionsCount(outputInfo.value, &rank));
        std::array<std::int64_t, 4> outputShape{};
        check(api, api.GetDimensions(outputInfo.value, outputShape.data(), outputShape.size()));
        std::size_t elementCount = 0;
        check(api, api.GetTensorShapeElementCount(outputInfo.value, &elementCount));
        validateStdfCudaOutputTensorContract(
            StdfCudaOutputTensorMetadata{
                valueType == ONNX_TYPE_TENSOR,
                dtype == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                rank,
                outputShape,
                elementCount},
            shape[2], shape[3], output.size());
        float* data = nullptr; check(api, api.GetTensorMutableData(outputValue.value, reinterpret_cast<void**>(&data)));
        if (!data) throw StdfError("STDF CUDA graph returned no output data");
        if (config_.enableTiling && data != output.data())
            throw StdfError("STDF CUDA tiled output did not retain its pre-budgeted host prediction buffer");
        if (data != output.data()) std::copy_n(data, output.size(), output.data());
        if (profiling_ && !profileChecked_) {
            finishProfile();
        }
    }
    StdfCudaConfig config_;
    inference::OrtRuntimeLease lease_;
    std::optional<validation::stdf::VerifiedWeights> model_;
    Handle<OrtSession> session_;
    StdfCudaDiagnostics diagnostics_;
    std::filesystem::path profilePrefix_;
    bool profiling_{};
    bool profileChecked_{};
    StdfCudaProfileGateState profileGate_{};
};

StdfCudaBackend::StdfCudaBackend(StdfCudaConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
StdfCudaBackend::~StdfCudaBackend() = default;
void StdfCudaBackend::loadModel(const std::filesystem::path& path) { impl_->load(path); }
media::DecodedFrame StdfCudaBackend::restore(const Request& request, std::span<const validation::stdf::BoundedPlanes> extents,
                                             const validation::stdf::SceneBounds& scene, float strength) {
    return impl_->restore(request, extents, scene, strength);
}
const StdfCudaDiagnostics& StdfCudaBackend::diagnostics() const noexcept { return impl_->diagnostics(); }
std::filesystem::path StdfCudaBackend::finishProfiling() { return impl_->finishProfile(); }

class StdfCudaVideoRestorationBackend::Impl final {
public:
    struct Job {
        Request request;
        std::promise<Result> promise;
    };

    explicit Impl(StdfCudaConfig config) : config_(std::move(config)) {
        if (config_.maxQueuedRequests == 0 || config_.maxQueuedRequests > 8 ||
            !std::isfinite(config_.strength) || config_.strength < 0.0F || config_.strength > 1.0F)
            throw std::invalid_argument("Invalid STDF strength or serialized queue limit");
        if (!config_.requireCriticalCudaProfile)
            throw std::invalid_argument("STDF video restoration requires critical CUDA profile verification");
        worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
    }

    ~Impl() {
        worker_.request_stop();
        ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Capabilities capabilities() const {
        std::lock_guard lock(mutex_);
        return capabilities_;
    }

    void loadModel(const ModelPack& model) {
        if (!supportsStdfCudaModel(model))
            throw std::invalid_argument("STDF model pack is not the strictly qualified local graph/adapter/capability identity");
        validateCapabilities(model.capabilities);
        auto candidate = std::make_shared<StdfCudaBackend>(config_);
        candidate->loadModel(model.graphPath);
        const auto diagnostics = candidate->diagnostics();
        if (!diagnostics.cudaProviderAvailable || !diagnostics.cudaSessionCreated)
            throw std::runtime_error("STDF CUDA provider/session was not created");
        auto candidateCapabilities = stdfCapabilities(model, diagnostics, config_);
        std::lock_guard lock(mutex_);
        if (active_ || !jobs_.empty()) throw std::runtime_error("Cannot replace an STDF model while requests are active");
        runner_ = std::move(candidate);
        model_ = model;
        capabilities_ = std::move(candidateCapabilities);
    }

    std::future<Result> submit(Request request) {
        std::lock_guard lock(mutex_);
        if (!runner_ || !model_)
            throw std::runtime_error("Load a strictly qualified STDF model before processing");
        if (!request.sceneBounds)
            throw std::invalid_argument("STDF restoration requests require explicit scene bounds");
        validateRequest(capabilities_, request);
        if (jobs_.size() >= config_.maxQueuedRequests)
            throw std::runtime_error("STDF restoration queue is full");
        Job job{std::move(request), {}};
        auto future = job.promise.get_future();
        jobs_.push_back(std::move(job));
        ready_.notify_one();
        return future;
    }

    std::filesystem::path finishProfiling() {
        std::lock_guard lock(mutex_);
        if (!runner_ || active_ || !jobs_.empty())
            throw std::runtime_error("Cannot finish STDF profiling while the backend is not idle");
        return runner_->finishProfiling();
    }

private:
    Result execute(Request request, std::stop_token workerStop) {
        std::stop_source combined;
        std::stop_callback requestCancellation(request.stop, [&] { combined.request_stop(); });
        std::stop_callback workerCancellation(workerStop, [&] { combined.request_stop(); });
        request.stop = combined.get_token();
        if (!request.sceneBounds) throw std::invalid_argument("STDF restoration requests require explicit scene bounds");
        validateRequest(capabilities_, request);
        const auto extents = provePlaneSpans(request);
        const auto scene = validation::stdf::SceneBounds{
            request.sceneBounds->firstIndex, request.sceneBounds->lastIndex, request.sceneBounds->serial};
        const auto source = request.context.at(request.centerIndex);
        const auto started = std::chrono::steady_clock::now();
        const auto output = runner_->restore(request, extents, scene, request.deblockStrength.value_or(config_.strength));
        Result result{output, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
        validateResult(capabilities_, request, result);
        for (const std::size_t plane : {std::size_t{1}, std::size_t{2}})
            if (result.centerFrame.planes[plane].data != source.planes[plane].data ||
                result.centerFrame.planes[plane].stride != source.planes[plane].stride)
                throw std::runtime_error("STDF changed the center U/V plane identity");
        const auto diagnostics = runner_->diagnostics();
        {
            std::lock_guard lock(mutex_);
            capabilities_ = stdfCapabilities(*model_, diagnostics, config_);
        }
        return result;
    }

    void work(std::stop_token stop) {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, stop, [this] { return !jobs_.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                active_ = true;
            }
            try {
                auto result = execute(std::move(job.request), stop);
                {
                    std::lock_guard lock(mutex_);
                    active_ = false;
                }
                job.promise.set_value(std::move(result));
            } catch (...) {
                const auto exception = std::current_exception();
                {
                    std::lock_guard lock(mutex_);
                    active_ = false;
                }
                job.promise.set_exception(exception);
            }
        }
        std::deque<Job> remaining;
        {
            std::lock_guard lock(mutex_);
            remaining.swap(jobs_);
        }
        for (auto& job : remaining)
            job.promise.set_exception(std::make_exception_ptr(std::runtime_error("STDF restoration backend stopped before execution")));
    }

    StdfCudaConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Job> jobs_;
    std::shared_ptr<StdfCudaBackend> runner_;
    std::optional<ModelPack> model_;
    Capabilities capabilities_{};
    bool active_{};
    std::jthread worker_;
};

StdfCudaVideoRestorationBackend::StdfCudaVideoRestorationBackend(StdfCudaConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
StdfCudaVideoRestorationBackend::~StdfCudaVideoRestorationBackend() = default;
Capabilities StdfCudaVideoRestorationBackend::capabilities() const { return impl_->capabilities(); }
void StdfCudaVideoRestorationBackend::loadModel(const ModelPack& model) { impl_->loadModel(model); }
std::future<Result> StdfCudaVideoRestorationBackend::restore(Request request) { return impl_->submit(std::move(request)); }
std::filesystem::path StdfCudaVideoRestorationBackend::finishProfiling() { return impl_->finishProfiling(); }

} // namespace rifelove::restoration

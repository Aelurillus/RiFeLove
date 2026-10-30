#include "inference/onnx_cuda_backend.hpp"
#include "inference/tensorrt_cache.hpp"
#ifdef RIFELOVE_HAS_MEDIA
#include "media/rife_color.hpp"
#include "restoration/onnx_cuda_backend.hpp"
#endif

// The pinned Microsoft header spells the Windows calling convention _stdcall.
// GCC in strict C++20 mode exposes __stdcall instead. Both use the x64 C ABI.
#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rifelove::inference {
namespace {
template<class T> struct Handle final {
    T* value{};
    void(ORT_API_CALL* release)(T*);
    explicit Handle(void(ORT_API_CALL* deleter)(T*)) : release(deleter) {}
    ~Handle() { if (value) release(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)), release(other.release) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (value) release(value); value = std::exchange(other.value, nullptr); release = other.release; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
template <typename Function>
Function exportedProc(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module,name);
    static_assert(sizeof(Function) == sizeof(address));
    return std::bit_cast<Function>(address);
}
class Runtime final {
public:
    explicit Runtime(const std::filesystem::path& directory) {
        std::error_code error;
        const auto path = std::filesystem::canonical(directory / "onnxruntime.dll",error);
        if (error) throw InferenceError(InferenceErrorKind::unavailable, "Native ONNX Runtime DLL is not present in the selected directory");
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) throw InferenceError(InferenceErrorKind::unavailable, "Could not load native ONNX Runtime DLL; Windows error " + std::to_string(GetLastError()));
        try {
            using GetApi = const OrtApiBase*(ORT_API_CALL*)() noexcept;
            const auto address = GetProcAddress(module_, "OrtGetApiBase");
            if (!address) throw InferenceError(InferenceErrorKind::unavailable, "ONNX Runtime C API is missing");
            const auto base = std::bit_cast<GetApi>(address)();
            if (std::string(base->GetVersionString()) != "1.22.0") throw InferenceError(InferenceErrorKind::unavailable, "Expected pinned ONNX Runtime 1.22.0");
            api = base->GetApi(ORT_API_VERSION);
            if (!api) throw InferenceError(InferenceErrorKind::unavailable, "ONNX Runtime API version is incompatible");
            directory_ = path.parent_path();
            dllDirectory_ = AddDllDirectory(directory_.c_str());
            if (!dllDirectory_) throw InferenceError(InferenceErrorKind::unavailable, "Could not add the selected runtime directory to the private DLL search path");
            // Load from the explicit SDK directory, including dependencies of
            // the lazily loaded provider. This needs no process PATH changes
            // and identifies the actual missing DLL before Windows error 126
            // can misleadingly report the first provider import instead.
            // cuDNN loads its split modules by basename when its API is first
            // invoked, so preload those as well, before any cudnnCreate call.
            constexpr const wchar_t* dependencies[] = {L"cudart64_12.dll", L"cublasLt64_12.dll", L"cublas64_12.dll", L"cufft64_11.dll",
                L"cudnn_graph64_9.dll", L"cudnn_ops64_9.dll", L"cudnn_cnn64_9.dll", L"cudnn_adv64_9.dll",
                L"cudnn_engines_precompiled64_9.dll", L"cudnn_engines_runtime_compiled64_9.dll", L"cudnn_heuristic64_9.dll",
                L"cudnn64_9.dll", L"onnxruntime_providers_shared.dll"};
            dependencyModules_.reserve(std::size(dependencies));
            for (const auto* name : dependencies) {
                const auto library = path.parent_path() / name;
                const auto loaded = LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!loaded) {
                    const std::wstring label(name);
                    throw InferenceError(InferenceErrorKind::unavailable, "Could not load native dependency " + std::string(label.begin(),label.end()) + "; Windows error " + std::to_string(GetLastError()));
                }
                dependencyModules_.push_back(loaded);
            }
            if (auto* status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,"RifeLove",&environment_)) {
                const std::string message = api->GetErrorMessage(status); api->ReleaseStatus(status);
                throw InferenceError(InferenceErrorKind::unavailable,"Could not initialize process ONNX environment: " + message);
            }
        } catch (...) { cleanup(); throw; }
    }
    ~Runtime() { cleanup(); }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    const OrtApi* api{};
    OrtEnv* environment() const { return environment_; }
    void ensureTensorRtProvider() const {
        const auto path = directory_ / L"onnxruntime_providers_tensorrt.dll";
        if (!std::filesystem::is_regular_file(path))
            throw InferenceError(InferenceErrorKind::unavailable, "ONNX Runtime TensorRT provider DLL is missing from the selected runtime directory");
    }
    void preloadTensorRtProviderDependencies() {
        if (tensorRtProviderDependenciesLoaded_) return;
        // TensorRT's builder opens its resource library by basename. An
        // AddDllDirectory entry is only searched by such default LoadLibrary
        // calls after the process opts into the safe default search path.
        // Keep that path limited to the application, explicitly registered
        // runtime directories, and System32; never depend on or mutate PATH.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            throw InferenceError(InferenceErrorKind::unavailable,
                "Could not enable the safe native DLL search path for TensorRT; Windows error " + std::to_string(GetLastError()));
        // Pin the selected runtime's matching libraries in this process first,
        // even when another ONNX Runtime installation is present on PATH.
        constexpr const wchar_t* dependencies[] = {
            L"nvinfer_plugin_10.dll", L"nvonnxparser_10.dll", L"nvinfer_builder_resource_10.dll"};
        for (const auto* name : dependencies) {
            const auto path = directory_ / name;
            const auto module = LoadLibraryExW(path.c_str(),nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) {
                const std::wstring label(name);
                throw InferenceError(InferenceErrorKind::unavailable,
                    "Could not load selected TensorRT dependency " + std::string(label.begin(),label.end()) +
                    "; Windows error " + std::to_string(GetLastError()));
            }
            dependencyModules_.push_back(module);
        }
        tensorRtProviderDependenciesLoaded_ = true;
    }
    std::string tensorRtVersion() {
        const auto path = directory_ / L"nvinfer_10.dll";
        const auto module = LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw InferenceError(InferenceErrorKind::unavailable,"Could not load TensorRT runtime; Windows error " + std::to_string(GetLastError()));
        using GetInferLibVersion = std::int32_t (*)();
        const auto getVersion = exportedProc<GetInferLibVersion>(module,"getInferLibVersion");
        if (!getVersion) {
            FreeLibrary(module);
            throw InferenceError(InferenceErrorKind::unavailable,"TensorRT runtime does not export getInferLibVersion");
        }
        dependencyModules_.push_back(module);
        const auto encoded = getVersion();
        if (encoded < 0) throw InferenceError(InferenceErrorKind::unavailable,"TensorRT runtime returned an invalid version");
        const auto major = encoded / 10000, minor = (encoded / 100) % 100, patch = encoded % 100;
        const auto apiVersion = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (major != 10 || minor != 9)
            throw InferenceError(InferenceErrorKind::unavailable,"Pinned ONNX Runtime 1.22 TensorRT provider requires TensorRT 10.9; found " + apiVersion);
        const auto manifest = directory_ / L"tensorrt-runtime.version";
        if (!std::filesystem::is_regular_file(manifest)) return apiVersion;
        const auto bytes = readLocalFile(manifest,256);
        const std::string content(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        std::istringstream lines(content);
        std::string packageVersion, wheelHash;
        std::getline(lines,packageVersion); std::getline(lines,wheelHash);
        if (!wheelHash.empty() && wheelHash.back() == '\r') wheelHash.pop_back();
        if (!packageVersion.starts_with(apiVersion + ".") && packageVersion != apiVersion)
            throw InferenceError(InferenceErrorKind::unavailable,"TensorRT runtime manifest disagrees with the loaded library API version");
        if (wheelHash.size() != 64 || !std::all_of(wheelHash.begin(),wheelHash.end(),[](unsigned char value) { return std::isxdigit(value) != 0; }))
            throw InferenceError(InferenceErrorKind::unavailable,"TensorRT runtime manifest is incomplete");
        return packageVersion;
    }
    std::string cudaRuntimeVersion() const {
        const auto path = directory_ / L"cudart64_12.dll";
        const auto module = LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw InferenceError(InferenceErrorKind::unavailable,"Could not load CUDA Runtime to identify the TensorRT engine cache version; Windows error " + std::to_string(GetLastError()));
        using RuntimeGetVersion = int (WINAPI*)(int*);
        const auto getVersion = exportedProc<RuntimeGetVersion>(module,"cudaRuntimeGetVersion");
        int version = 0;
        const int result = getVersion ? getVersion(&version) : -1;
        FreeLibrary(module);
        if (result != 0 || version < 1000)
            throw InferenceError(InferenceErrorKind::unavailable,"Could not query the installed CUDA Runtime version for TensorRT engine caching");
        return std::to_string(version / 1000) + "." + std::to_string((version % 1000) / 10);
    }
private:
    OrtEnv* environment_{};
    HMODULE module_{};
    std::vector<HMODULE> dependencyModules_;
    std::filesystem::path directory_;
    DLL_DIRECTORY_COOKIE dllDirectory_{};
    bool tensorRtProviderDependenciesLoaded_{};
    void cleanup() noexcept {
        if (environment_) { api->ReleaseEnv(environment_); environment_ = nullptr; }
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        for (auto iterator = dependencyModules_.rbegin(); iterator != dependencyModules_.rend(); ++iterator) FreeLibrary(*iterator);
        dependencyModules_.clear();
        if (dllDirectory_) { RemoveDllDirectory(dllDirectory_); dllDirectory_ = nullptr; }
    }
};
std::filesystem::path defaultEngineCacheDirectory() {
    std::array<wchar_t,32768> value{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA",value.data(),static_cast<DWORD>(value.size()));
    if (size && size < value.size()) return std::filesystem::path(value.data()) / L"RifeLove" / L"Cache" / L"TensorRT";
    return std::filesystem::temp_directory_path() / L"RifeLove" / L"Cache" / L"TensorRT";
}
std::string utf8Path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(),value.end()};
}
std::shared_ptr<Runtime> processRuntime(const std::filesystem::path& selected) {
    struct Registry { std::mutex mutex; std::filesystem::path directory; std::shared_ptr<Runtime> runtime; };
    static Registry registry;
    std::error_code error;
    const auto directory = std::filesystem::canonical(selected,error);
    if (error) throw InferenceError(InferenceErrorKind::unavailable,"Native ONNX Runtime directory is unavailable");
    std::lock_guard lock(registry.mutex);
    if (registry.runtime) {
        if (!std::filesystem::equivalent(directory,registry.directory))
            throw InferenceError(InferenceErrorKind::unavailable,"Restart RifeLove before changing the native ONNX/CUDA runtime directory");
        return registry.runtime;
    }
    auto runtime = std::make_shared<Runtime>(directory);
    registry.directory = directory; registry.runtime = runtime; return runtime;
}
void check(const OrtApi& api, OrtStatus* status, std::stop_token stop = {}, std::stop_token shutdown = {}) {
    if (!status) return;
    const std::string message = api.GetErrorMessage(status);
    api.ReleaseStatus(status);
    auto kind = InferenceErrorKind::runtimeError;
    if (stop.stop_requested() || shutdown.stop_requested()) kind = InferenceErrorKind::cancelled;
    else if (message.find("out of memory") != std::string::npos || message.find("Failed to allocate memory") != std::string::npos) kind = InferenceErrorKind::outOfMemory;
    throw InferenceError(kind, message);
}
void cancelled(std::stop_token stop, std::stop_token shutdown = {}) {
    if (stop.stop_requested() || shutdown.stop_requested()) throw InferenceError(InferenceErrorKind::cancelled, "Interpolation cancelled");
}
std::size_t frameBytes(int width, int height, int multiple) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || width % multiple || height % multiple)
        throw InferenceError(InferenceErrorKind::invalidRequest, "RIFE dimensions must be positive multiples of 64, at most 8192");
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 * sizeof(float);
}
void validateContract(const OrtApi& api, const OrtSession* session) {
    std::size_t inputs = 0, outputs = 0;
    check(api, api.SessionGetInputCount(session, &inputs));
    check(api, api.SessionGetOutputCount(session, &outputs));
    if (inputs != 3 || outputs != 1) throw InferenceError(InferenceErrorKind::invalidRequest, "Expected three RIFE inputs and one output");
    OrtAllocator* allocator = nullptr;
    check(api, api.GetAllocatorWithDefaultOptions(&allocator));
    constexpr const char* names[] = {"frame0", "frame1", "timestep", "frame"};
    for (std::size_t index = 0; index < 4; ++index) {
        const bool output = index == 3;
        char* name = nullptr;
        check(api, output ? api.SessionGetOutputName(session, 0, allocator, &name) : api.SessionGetInputName(session, index, allocator, &name));
        const bool expectedName = name && names[index] == std::string(name);
        allocator->Free(allocator, name);
        if (!expectedName) throw InferenceError(InferenceErrorKind::invalidRequest, "RIFE tensor names/order do not match the adapter");
        Handle<OrtTypeInfo> type(api.ReleaseTypeInfo);
        check(api, output ? api.SessionGetOutputTypeInfo(session, 0, &type.value) : api.SessionGetInputTypeInfo(session, index, &type.value));
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        check(api, api.CastTypeInfoToTensorInfo(type.value, &tensor));
        ONNXTensorElementDataType dtype{};
        check(api, api.GetTensorElementType(tensor, &dtype));
        std::size_t rank = 0;
        check(api, api.GetDimensionsCount(tensor, &rank));
        if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != 4) throw InferenceError(InferenceErrorKind::invalidRequest, "Expected FP32 rank-four RIFE tensors");
        std::array<std::int64_t, 4> dimensions{};
        check(api, api.GetDimensions(tensor, dimensions.data(), dimensions.size()));
        const auto dynamic = [](std::int64_t dimension) { return dimension == 0 || dimension == -1; };
        const bool timestep = index == 2;
        const bool valid = timestep ? dimensions == std::array<std::int64_t,4>{1,1,1,1} :
            dimensions[0] == 1 && (dimensions[1] == 3 || (output && dynamic(dimensions[1]))) &&
            dynamic(dimensions[2]) && dynamic(dimensions[3]);
        if (!valid) throw InferenceError(InferenceErrorKind::invalidRequest, "RIFE shape contract requires a batch-one RGB tensor, dynamic spatial axes and scalar timestep");
    }
}
}

class OnnxCudaBackend::Impl final {
public:
    struct Job { InterpolationRequest request; std::promise<InterpolationResult> promise; };
    Impl(std::shared_ptr<gpu::CudaDevice> selected, OnnxCudaConfig settings)
        : device(std::move(selected)), config(std::move(settings)), runtime(processRuntime(config.runtimeDirectory)), api(*runtime->api),
          session(api.ReleaseSession), cudaMemory(api.ReleaseMemoryInfo) {
        if (!device || !config.arenaBudgetBytes || !config.tensorRtWorkspaceBytes || !config.maxQueuedRequests || config.maxQueuedRequests > 64)
            throw InferenceError(InferenceErrorKind::invalidRequest, "Invalid CUDA backend device, budget or queue limit");
        if (config.enableTensorRtFp16 && config.providerPreference != ProviderPreference::requireTensorRt)
            throw InferenceError(InferenceErrorKind::invalidRequest,
                "TensorRT FP16 requires the TensorRT provider; silent fallback to CUDA FP32 is disabled");
        if (config.engineCacheDirectory.empty()) config.engineCacheDirectory = defaultEngineCacheDirectory();
        check(api, api.CreateMemoryInfo("Cuda", OrtDeviceAllocator, device->info().ordinal, OrtMemTypeDefault, &cudaMemory.value));
        timestep = device->allocate(sizeof(float));
        worker = std::jthread([this](std::stop_token stop) { work(stop); });
    }
    ~Impl() {
        worker.request_stop();
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }
    void load(const ModelPack& model) {
        std::lock_guard lock(mutex);
        if (active || !jobs.empty()) throw InferenceError(InferenceErrorKind::busy, "Cannot replace a model while requests are active");
        if (model.spatialMultiple != 64 || model.graph.empty() || sha256(model.graph) != model.sha256)
            throw InferenceError(InferenceErrorKind::invalidRequest, "Model bytes or spatial contract failed verification");
        auto context = device->activate();
        fallbackReason.clear();
        resolvedBackend = "onnxruntime_cuda";
        const auto buildSession = [&](bool useTensorRt) {
            Handle<OrtSessionOptions> options(api.ReleaseSessionOptions);
            check(api, api.CreateSessionOptions(&options.value));
            check(api, api.SetSessionExecutionMode(options.value, ORT_SEQUENTIAL));
            check(api, api.SetIntraOpNumThreads(options.value, 2));
            check(api, api.SetInterOpNumThreads(options.value, 1));
            check(api, api.SetSessionGraphOptimizationLevel(options.value, ORT_ENABLE_EXTENDED));
            if (!config.profilePrefix.empty()) check(api, api.EnableProfiling(options.value, config.profilePrefix.c_str()));
            if (useTensorRt) {
                runtime->ensureTensorRtProvider();
                const auto version = runtime->tensorRtVersion();
                runtime->preloadTensorRtProviderDependencies();
                const auto deviceInfo = device->info();
                const auto cudaVersion = runtime->cudaRuntimeVersion();
                const TensorRtCacheIdentity identity{"1.22.0",version,cudaVersion,
                    config.enableTensorRtFp16 ? "fp16" : "fp32","rife-v1-min64-opt720p-max4k",config.tensorRtWorkspaceBytes};
                const auto prefix = tensorRtEngineCachePrefix(model,deviceInfo,identity);
                const auto cacheDirectory = config.engineCacheDirectory / prefix;
                std::error_code error;
                std::filesystem::create_directories(cacheDirectory,error);
                if (error) throw InferenceError(InferenceErrorKind::unavailable,"Could not create the local TensorRT engine cache directory: " + error.message());
                const auto cachePath = utf8Path(cacheDirectory);
                Handle<OrtTensorRTProviderOptionsV2> tensorRt(api.ReleaseTensorRTProviderOptions);
                check(api,api.CreateTensorRTProviderOptions(&tensorRt.value));
                const std::string tensorRtOrdinal = std::to_string(deviceInfo.ordinal);
                const std::string tensorRtWorkspace = std::to_string(config.tensorRtWorkspaceBytes);
                const char* profileMin = "frame0:1x3x64x64,frame1:1x3x64x64";
                const char* profileOpt = "frame0:1x3x768x1280,frame1:1x3x768x1280";
                const char* profileMax = "frame0:1x3x2176x3840,frame1:1x3x2176x3840";
                const char* fp16Enabled = config.enableTensorRtFp16 ? "1" : "0";
                const char* tensorRtKeys[] = {"device_id","trt_engine_cache_enable","trt_engine_cache_path","trt_engine_cache_prefix",
                    "trt_fp16_enable","trt_max_workspace_size","trt_profile_min_shapes","trt_profile_opt_shapes","trt_profile_max_shapes"};
                // The directory already binds the full cache identity. Reusing
                // its long hash as the filename prefix can exceed Windows'
                // path limit once ORT appends its subgraph and GPU suffixes.
                const char* tensorRtValues[] = {tensorRtOrdinal.c_str(),"1",cachePath.c_str(),"trt",fp16Enabled,tensorRtWorkspace.c_str(),
                    profileMin,profileOpt,profileMax};
                check(api,api.UpdateTensorRTProviderOptions(tensorRt.value,tensorRtKeys,tensorRtValues,std::size(tensorRtKeys)));
                check(api,api.SessionOptionsAppendExecutionProvider_TensorRT_V2(options.value,tensorRt.value));
            }
            Handle<OrtCUDAProviderOptionsV2> cuda(api.ReleaseCUDAProviderOptions);
            check(api, api.CreateCUDAProviderOptions(&cuda.value));
            const std::string ordinal = std::to_string(device->info().ordinal), budget = std::to_string(config.arenaBudgetBytes);
            const char* keys[] = {"device_id", "gpu_mem_limit", "arena_extend_strategy", "cudnn_conv_algo_search", "cudnn_conv_use_max_workspace", "use_tf32", "do_copy_in_default_stream"};
            // Use the documented implicit-precomputed-GEMM baseline. Heuristic
            // selection showed a gate failure on a repeated native run; faster
            // algorithms need their own numerical qualification before use.
            const char* values[] = {ordinal.c_str(), budget.c_str(), "kSameAsRequested", "DEFAULT", "0", "0", "1"};
            check(api, api.UpdateCUDAProviderOptions(cuda.value, keys, values, std::size(keys)));
            check(api, api.UpdateCUDAProviderOptionsWithValue(cuda.value, "user_compute_stream", reinterpret_cast<void*>(device->streamAddress())));
            try { check(api, api.SessionOptionsAppendExecutionProvider_CUDA_V2(options.value, cuda.value)); }
            catch (const InferenceError& error) { throw InferenceError(InferenceErrorKind::unavailable, "CUDA provider could not initialize: " + std::string(error.what())); }
            Handle<OrtSession> next(api.ReleaseSession);
            check(api, api.CreateSessionFromArray(runtime->environment(), model.graph.data(), model.graph.size(), options.value, &next.value));
            validateContract(api, next.value);
            return next;
        };
        if (config.providerPreference != ProviderPreference::cudaOnly) {
            try {
                session = buildSession(true);
                resolvedBackend = "onnxruntime_tensorrt_cuda";
            } catch (const InferenceError& error) {
                if (config.providerPreference == ProviderPreference::requireTensorRt) throw;
                fallbackReason = error.what();
            }
        }
        if (!session.value) session = buildSession(false);
        modelId = model.id;
        adapterId = model.adapterId;
        adapterVersion = model.adapterVersion;
        supportedInputProfiles = model.supportedInputProfiles;
        profiled = !config.profilePrefix.empty();
    }
    std::future<InterpolationResult> submit(InterpolationRequest request) {
        std::lock_guard lock(mutex);
        if (!session.value) throw InferenceError(InferenceErrorKind::invalidRequest, "Load a verified model before interpolation");
        if (jobs.size() >= config.maxQueuedRequests) throw InferenceError(InferenceErrorKind::busy, "Interpolation queue is full");
        Job job{std::move(request), {}};
        auto future = job.promise.get_future();
        jobs.push_back(std::move(job));
        ready.notify_one();
        return future;
    }
    std::filesystem::path finishProfile() {
        std::lock_guard lock(mutex);
        if (active || !jobs.empty()) throw InferenceError(InferenceErrorKind::busy, "Cannot finish profiling while inference is active");
        if (!session.value || !profiled) throw InferenceError(InferenceErrorKind::invalidRequest, "No active ONNX profile");
        OrtAllocator* allocator = nullptr;
        check(api, api.GetAllocatorWithDefaultOptions(&allocator));
        char* name = nullptr;
        check(api, api.SessionEndProfiling(session.value, allocator, &name));
        const std::string utf8 = name ? name : "";
        const auto path = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        allocator->Free(allocator, name);
        profiled = false;
        return path;
    }
    ModelCapabilities capabilities() const {
        std::lock_guard lock(mutex);
        const bool fp16 = config.enableTensorRtFp16 && resolvedBackend == "onnxruntime_tensorrt_cuda";
        return {resolvedBackend, modelId, 64, true, fp16, fallbackReason, supportedInputProfiles, adapterId, adapterVersion};
    }
private:
    InterpolationResult execute(InterpolationRequest& request, std::stop_token shutdown) {
        const auto start = std::chrono::steady_clock::now();
        cancelled(request.stop, shutdown);
        const auto bytes = frameBytes(request.frame0.width, request.frame0.height, 64);
        if (request.frame1.width != request.frame0.width || request.frame1.height != request.frame0.height ||
            !std::isfinite(request.timestep) || request.timestep < 0 || request.timestep > 1)
            throw InferenceError(InferenceErrorKind::invalidRequest, "Mismatched input shapes or invalid timestep");
        for (const auto* frame : {&request.frame0, &request.frame1}) {
            if (!frame->pixels || frame->pixels->size() != bytes || frame->pixels->deviceOrdinal() != device->info().ordinal)
                throw InferenceError(InferenceErrorKind::invalidRequest, "Expected owned FP32 RGB device frames on the selected GPU");
        }
        const bool reused = static_cast<bool>(request.output);
        if (reused && (request.output.use_count() != 1 || request.output->size() != bytes || request.output->deviceOrdinal() != device->info().ordinal ||
            request.output->address() == request.frame0.pixels->address() || request.output->address() == request.frame1.pixels->address()))
            throw InferenceError(InferenceErrorKind::invalidRequest, "Reusable output must be exclusively owned, correctly sized and distinct from inputs");
        auto output = reused ? std::move(request.output) : device->allocate(bytes, request.stop);
        auto context = device->activate();
        if (request.timestep == 0 || request.timestep == 1) {
            device->copy(*(request.timestep == 0 ? request.frame0.pixels : request.frame1.pixels), *output, bytes, request.stop);
        } else {
            timestep->upload(std::as_bytes(std::span(&request.timestep, 1)), 0, request.stop);
            Handle<OrtValue> frame0(api.ReleaseValue), frame1(api.ReleaseValue), time(api.ReleaseValue), frame(api.ReleaseValue);
            const std::int64_t shape[] = {1,3,request.frame0.height,request.frame0.width};
            const std::int64_t scalar[] = {1,1,1,1};
            const auto tensor = [&](Handle<OrtValue>& value, std::uintptr_t address, std::size_t length, const std::int64_t* dimensions) {
                check(api, api.CreateTensorWithDataAsOrtValue(cudaMemory.value, reinterpret_cast<void*>(address), length, dimensions, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value.value));
            };
            tensor(frame0, request.frame0.pixels->address(), bytes, shape);
            tensor(frame1, request.frame1.pixels->address(), bytes, shape);
            tensor(time, timestep->address(), sizeof(float), scalar);
            tensor(frame, output->address(), bytes, shape);
            Handle<OrtIoBinding> binding(api.ReleaseIoBinding);
            check(api, api.CreateIoBinding(session.value, &binding.value));
            check(api, api.BindInput(binding.value, "frame0", frame0.value));
            check(api, api.BindInput(binding.value, "frame1", frame1.value));
            check(api, api.BindInput(binding.value, "timestep", time.value));
            check(api, api.BindOutput(binding.value, "frame", frame.value));
            Handle<OrtRunOptions> options(api.ReleaseRunOptions);
            check(api, api.CreateRunOptions(&options.value));
            auto terminate = [&] { if (auto status = api.RunOptionsSetTerminate(options.value)) api.ReleaseStatus(status); };
            std::stop_callback requested(request.stop, terminate);
            std::stop_callback stopping(shutdown, terminate);
            try {
                check(api, api.RunWithBinding(session.value, options.value, binding.value), request.stop, shutdown);
                check(api, api.SynchronizeBoundOutputs(binding.value), request.stop, shutdown);
                device->synchronize();
            } catch (...) {
                // Even cancellation/error must finish queued GPU work before
                // input, timestep and output storage can be released/reused.
                try { device->synchronize(); } catch (...) {}
                throw;
            }
        }
        cancelled(request.stop, shutdown);
        return {{std::move(output),request.frame0.width,request.frame0.height},
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), reused};
    }
    void work(std::stop_token stop) {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, stop, [this] { return !jobs.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(jobs.front()); jobs.pop_front(); active = true;
            }
            try {
                auto result = execute(job.request, stop);
                { std::lock_guard lock(mutex); active = false; }
                job.promise.set_value(std::move(result));
            } catch (...) {
                auto exception = std::current_exception();
                try { std::rethrow_exception(exception); }
                catch (const gpu::GpuError& error) {
                    auto kind = InferenceErrorKind::runtimeError;
                    if (error.kind() == gpu::GpuErrorKind::outOfMemory) kind = InferenceErrorKind::outOfMemory;
                    else if (error.kind() == gpu::GpuErrorKind::cancelled) kind = InferenceErrorKind::cancelled;
                    else if (error.kind() == gpu::GpuErrorKind::invalidRequest) kind = InferenceErrorKind::invalidRequest;
                    else if (error.kind() == gpu::GpuErrorKind::unavailable) kind = InferenceErrorKind::unavailable;
                    exception = std::make_exception_ptr(InferenceError(kind, error.what()));
                } catch (...) {}
                { std::lock_guard lock(mutex); active = false; }
                job.promise.set_exception(exception);
            }
        }
        std::lock_guard lock(mutex);
        for (auto& job : jobs) job.promise.set_exception(std::make_exception_ptr(InferenceError(InferenceErrorKind::cancelled, "Backend stopped before request execution")));
        jobs.clear();
    }
    std::shared_ptr<gpu::CudaDevice> device;
    OnnxCudaConfig config;
    std::shared_ptr<Runtime> runtime;
    const OrtApi& api;
    Handle<OrtSession> session;
    Handle<OrtMemoryInfo> cudaMemory;
    std::shared_ptr<gpu::DeviceBuffer> timestep;
    mutable std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Job> jobs;
    bool active = false;
    bool profiled = false;
    std::string modelId;
    std::string adapterId = "rife-sdr-v1";
    int adapterVersion = 1;
    std::vector<std::string> supportedInputProfiles;
    std::string resolvedBackend = "onnxruntime_cuda";
    std::string fallbackReason;
    std::jthread worker;
};
OnnxCudaBackend::OnnxCudaBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaConfig config) : impl_(std::make_unique<Impl>(std::move(device), std::move(config))) {}
OnnxCudaBackend::~OnnxCudaBackend() = default;
ModelCapabilities OnnxCudaBackend::capabilities() const { return impl_->capabilities(); }
void OnnxCudaBackend::loadModel(const ModelPack& model) { impl_->load(model); }
std::future<InterpolationResult> OnnxCudaBackend::interpolate(InterpolationRequest request) { return impl_->submit(std::move(request)); }
std::filesystem::path OnnxCudaBackend::finishProfiling() { return impl_->finishProfile(); }
} // namespace rifelove::inference

#ifdef RIFELOVE_HAS_MEDIA
namespace rifelove::restoration {
namespace {
std::size_t tensorBytes(int width, int height, std::size_t limit, const char* label, std::size_t channels = 3) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest, std::string(label) + " dimensions are outside the supported limit");
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > std::numeric_limits<std::size_t>::max() / (channels * sizeof(float)))
        throw inference::InferenceError(inference::InferenceErrorKind::outOfMemory, std::string(label) + " tensor size overflows addressable memory");
    const auto bytes = static_cast<std::size_t>(pixels) * channels * sizeof(float);
    if (bytes > limit)
        throw inference::InferenceError(inference::InferenceErrorKind::outOfMemory, std::string(label) + " tensor exceeds the configured host buffer limit");
    return bytes;
}

std::pair<std::string,std::array<std::int64_t,4>> inspectTensor(const OrtApi& api, const OrtSession* session, bool output, int inputChannels = 3) {
    OrtAllocator* allocator = nullptr;
    inference::check(api, api.GetAllocatorWithDefaultOptions(&allocator));
    char* name = nullptr;
    inference::check(api, output ? api.SessionGetOutputName(session,0,allocator,&name) : api.SessionGetInputName(session,0,allocator,&name));
    if (!name) throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX tensor has no name");
    std::string copiedName(name);
    allocator->Free(allocator,name);
    inference::Handle<OrtTypeInfo> type(api.ReleaseTypeInfo);
    inference::check(api, output ? api.SessionGetOutputTypeInfo(session,0,&type.value) : api.SessionGetInputTypeInfo(session,0,&type.value));
    const OrtTensorTypeAndShapeInfo* tensor = nullptr;
    inference::check(api, api.CastTypeInfoToTensorInfo(type.value,&tensor));
    ONNXTensorElementDataType elementType{};
    inference::check(api, api.GetTensorElementType(tensor,&elementType));
    std::size_t rank = 0;
    inference::check(api, api.GetDimensionsCount(tensor,&rank));
    std::array<std::int64_t,4> dimensions{};
    if (elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != dimensions.size())
        throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX graph must use one rank-four FP32 tensor");
    inference::check(api, api.GetDimensions(tensor,dimensions.data(),dimensions.size()));
    const auto dynamic = [](std::int64_t dimension) { return dimension == 0 || dimension == -1; };
    if ((dimensions[0] != 1 && !dynamic(dimensions[0])) || (dimensions[1] != (output ? 3 : inputChannels) && !(output && dynamic(dimensions[1]))))
        throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX graph must accept RGB NCHW batch-one tensors");
    return {std::move(copiedName),dimensions};
}
std::filesystem::path restorationEngineCacheDirectory() {
    std::array<wchar_t,32768> value{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA",value.data(),static_cast<DWORD>(value.size()));
    if (size && size < value.size()) return std::filesystem::path(value.data()) / L"RifeLove" / L"Cache" / L"TensorRT";
    return std::filesystem::temp_directory_path() / L"RifeLove" / L"Cache" / L"TensorRT";
}
}

class OnnxCudaVideoRestorationBackend::Impl final {
public:
    struct Job { Request request; std::promise<Result> promise; };

    Impl(std::shared_ptr<gpu::CudaDevice> selected, OnnxCudaRestorationConfig settings)
        : device(std::move(selected)), config(std::move(settings)), runtime(inference::processRuntime(config.runtimeDirectory)),
          api(*runtime->api), session(api.ReleaseSession), cudaMemory(api.ReleaseMemoryInfo) {
        if (!device || config.arenaBudgetBytes == 0 || config.hostFloatBufferLimit == 0 ||
            config.maxQueuedRequests == 0 || config.maxQueuedRequests > 8 || config.tileSize < 64 || config.tileSize > 2048 ||
            config.tileOverlap < 0 || config.tileOverlap > config.tileSize / 2)
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Invalid restoration CUDA device, memory budget or queue limit");
        auto context = device->activate();
        inference::check(api,api.CreateMemoryInfo("Cuda",OrtDeviceAllocator,device->info().ordinal,OrtMemTypeDefault,&cudaMemory.value));
        worker = std::jthread([this](std::stop_token stop) { work(stop); });
    }

    ~Impl() {
        worker.request_stop();
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }

    void load(const ModelPack& pack) {
        std::lock_guard lock(mutex);
        if (active || !jobs.empty()) throw inference::InferenceError(inference::InferenceErrorKind::busy,"Cannot replace a restoration model while requests are active");
        validateCapabilities(pack.capabilities);
        const auto task = outputRateImageTask(pack.capabilities);
        const bool deblurAdapter = task == Task::deblur;
        const bool temporalAdapter = supportsOnnxCudaTemporalModel(pack);
        const int nextChannels = temporalAdapter ? 16 : 3;
        if (!supportsOnnxCudaOutputRateModel(pack) && !temporalAdapter)
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Model does not match a supported SDR restoration adapter");

        const auto graph = inference::readVerifiedFile(pack.graphPath,pack.graphSha256,2ULL * 1024 * 1024 * 1024);
        auto context = device->activate();

        // Read the dynamic input name/shape with a lightweight CPU-only session
        // before constructing TensorRT's explicit optimization profile.
        inference::Handle<OrtSessionOptions> inspectOptions(api.ReleaseSessionOptions);
        inference::check(api,api.CreateSessionOptions(&inspectOptions.value));
        inference::check(api,api.SetSessionGraphOptimizationLevel(inspectOptions.value,ORT_DISABLE_ALL));
        inference::Handle<OrtSession> inspectSession(api.ReleaseSession);
        inference::check(api,api.CreateSessionFromArray(runtime->environment(),graph.data(),graph.size(),inspectOptions.value,&inspectSession.value));
        std::size_t inputs = 0, outputs = 0;
        inference::check(api,api.SessionGetInputCount(inspectSession.value,&inputs));
        inference::check(api,api.SessionGetOutputCount(inspectSession.value,&outputs));
        if (inputs != 1 || outputs != 1)
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX graph must expose exactly one input and one output");
        const auto [nextInputName,nextInputShape] = inspectTensor(api,inspectSession.value,false,nextChannels);
        const auto [nextOutputName,nextOutputShape] = inspectTensor(api,inspectSession.value,true);
        if (nextInputShape[2] > 0 || nextInputShape[3] > 0 || nextOutputShape[2] > 0 || nextOutputShape[3] > 0)
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Tiled restoration requires dynamic height and width axes");
        api.ReleaseSession(inspectSession.value);
        inspectSession.value = nullptr;

        const auto buildSession = [&](bool useTensorRt) {
            inference::Handle<OrtSessionOptions> options(api.ReleaseSessionOptions);
            inference::check(api,api.CreateSessionOptions(&options.value));
            inference::check(api,api.SetSessionExecutionMode(options.value,ORT_SEQUENTIAL));
            inference::check(api,api.SetIntraOpNumThreads(options.value,2));
            inference::check(api,api.SetInterOpNumThreads(options.value,1));
            inference::check(api,api.SetSessionGraphOptimizationLevel(options.value,ORT_ENABLE_EXTENDED));
            if (!config.profilePrefix.empty()) inference::check(api,api.EnableProfiling(options.value,config.profilePrefix.c_str()));
            if (useTensorRt) {
                runtime->ensureTensorRtProvider();
                const auto trtVersion = runtime->tensorRtVersion();
                runtime->preloadTensorRtProviderDependencies();
                const auto deviceInfo = device->info();
                const auto cudaVersion = runtime->cudaRuntimeVersion();
                // The Zoo NAFNet graph's traced local-pooling window requires
                // at least 384 pixels on each axis; smaller tiles are padded.
                const int minProfile = deblurAdapter ? 384 : 64;
                const int coreSize = std::max(config.tileSize,minProfile);
                // NAFNet's local attention needs substantially more context
                // than the image-SR models when an image spans multiple tiles.
                const int overlap = deblurAdapter ? std::max(config.tileOverlap,192) : temporalAdapter ? std::max(config.tileOverlap,64) : config.tileOverlap;
                const int maxProfile = coreSize + 2 * overlap;
                const int optProfile = coreSize;
                const std::string profileIdentity = (deblurAdapter ? "nafnet-rgb-fp32-min384-opt" : temporalAdapter ? "fastdvdnet-nchw16-fp32-min64-opt" : "sr-rgb-fp32-min64-opt") +
                    std::to_string(optProfile) + "-max" + std::to_string(maxProfile);
                const inference::TensorRtCacheIdentity identity{"1.22.0",trtVersion,cudaVersion,"fp32",profileIdentity,512ULL * 1024 * 1024};
                const auto prefix = inference::tensorRtEngineCachePrefix("restoration",pack.id,pack.version,pack.graphSha256,deviceInfo,identity);
                auto cacheRoot = config.engineCacheDirectory.empty() ? restorationEngineCacheDirectory() : config.engineCacheDirectory;
                const auto cacheDirectory = cacheRoot / prefix;
                std::error_code error;
                std::filesystem::create_directories(cacheDirectory,error);
                if (error) throw inference::InferenceError(inference::InferenceErrorKind::unavailable,
                    "Could not create the local TensorRT restoration cache: " + error.message());
                const auto cachePath = inference::utf8Path(cacheDirectory);
                inference::Handle<OrtTensorRTProviderOptionsV2> tensorRt(api.ReleaseTensorRTProviderOptions);
                inference::check(api,api.CreateTensorRTProviderOptions(&tensorRt.value));
                const std::string tensorRtOrdinal = std::to_string(deviceInfo.ordinal);
                const std::string workspace = std::to_string(identity.workspaceBytes);
                const auto tensorPrefix = nextInputName + ":1x" + std::to_string(nextChannels) + "x";
                const std::string minShapes = tensorPrefix + std::to_string(minProfile) + "x" + std::to_string(minProfile);
                const std::string optShapes = tensorPrefix + std::to_string(optProfile) + "x" + std::to_string(optProfile);
                const std::string maxShapes = tensorPrefix + std::to_string(maxProfile) + "x" + std::to_string(maxProfile);
                const char* trtKeys[] = {"device_id","trt_engine_cache_enable","trt_engine_cache_path","trt_engine_cache_prefix",
                    "trt_fp16_enable","trt_max_workspace_size","trt_profile_min_shapes","trt_profile_opt_shapes","trt_profile_max_shapes"};
                const char* trtValues[] = {tensorRtOrdinal.c_str(),"1",cachePath.c_str(),"trt","0",workspace.c_str(),
                    minShapes.c_str(),optShapes.c_str(),maxShapes.c_str()};
                inference::check(api,api.UpdateTensorRTProviderOptions(tensorRt.value,trtKeys,trtValues,std::size(trtKeys)));
                inference::check(api,api.SessionOptionsAppendExecutionProvider_TensorRT_V2(options.value,tensorRt.value));
            }
            inference::Handle<OrtCUDAProviderOptionsV2> cuda(api.ReleaseCUDAProviderOptions);
            inference::check(api,api.CreateCUDAProviderOptions(&cuda.value));
            const std::string ordinal = std::to_string(device->info().ordinal), budget = std::to_string(config.arenaBudgetBytes);
            const char* keys[] = {"device_id","gpu_mem_limit","arena_extend_strategy","cudnn_conv_algo_search","cudnn_conv_use_max_workspace","use_tf32","do_copy_in_default_stream"};
            const char* values[] = {ordinal.c_str(),budget.c_str(),"kSameAsRequested","DEFAULT","0","0","1"};
            inference::check(api,api.UpdateCUDAProviderOptions(cuda.value,keys,values,std::size(keys)));
            inference::check(api,api.UpdateCUDAProviderOptionsWithValue(cuda.value,"user_compute_stream",reinterpret_cast<void*>(device->streamAddress())));
            try { inference::check(api,api.SessionOptionsAppendExecutionProvider_CUDA_V2(options.value,cuda.value)); }
            catch (const inference::InferenceError& error) {
                throw inference::InferenceError(inference::InferenceErrorKind::unavailable,"CUDA restoration provider could not initialize: " + std::string(error.what()));
            }
            inference::Handle<OrtSession> next(api.ReleaseSession);
            inference::check(api,api.CreateSessionFromArray(runtime->environment(),graph.data(),graph.size(),options.value,&next.value));
            std::size_t actualInputs = 0, actualOutputs = 0;
            inference::check(api,api.SessionGetInputCount(next.value,&actualInputs));
            inference::check(api,api.SessionGetOutputCount(next.value,&actualOutputs));
            if (actualInputs != 1 || actualOutputs != 1)
                throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX graph must expose exactly one input and one output");
            const auto [actualInputName,actualInputShape] = inspectTensor(api,next.value,false,nextChannels);
            const auto [actualOutputName,actualOutputShape] = inspectTensor(api,next.value,true);
            if (actualInputName != nextInputName || actualInputShape != nextInputShape || actualOutputName != nextOutputName || actualOutputShape != nextOutputShape)
                throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration ONNX tensor contract changed while initializing an execution provider");
            return next;
        };

        inference::Handle<OrtSession> next(api.ReleaseSession);
        std::string selectedBackend = "onnxruntime_cuda";
        std::string selectedFallbackReason;
        if (config.providerPreference != inference::ProviderPreference::cudaOnly) {
            try {
                next = buildSession(true);
                selectedBackend = "onnxruntime_tensorrt_cuda";
            } catch (const inference::InferenceError& error) {
                if (config.providerPreference == inference::ProviderPreference::requireTensorRt) throw;
                selectedFallbackReason = error.what();
            }
        }
        if (!next.value) next = buildSession(false);
        const int minTile = deblurAdapter ? 384 : 64;
        const int coreSize = std::max(config.tileSize,minTile);
        const int overlap = deblurAdapter ? std::max(config.tileOverlap,192) : temporalAdapter ? std::max(config.tileOverlap,64) : config.tileOverlap;
        const std::string selectedSignature = "ort-1.22.0;cuda-" + runtime->cudaRuntimeVersion() +
            ";trt-" + (selectedBackend == "onnxruntime_tensorrt_cuda" ? runtime->tensorRtVersion() : std::string("none")) +
            ";fp32;tile-" + std::to_string(coreSize) + ";overlap-" + std::to_string(overlap) +
            (deblurAdapter ? ";nafnet-balanced-tiles-v1" : temporalAdapter ? ";fastdvdnet-five-frame-clamp-tiles-v1" : ";sr-tiles-v1");
        session = std::move(next);
        inputName = nextInputName;
        outputName = nextOutputName;
        inputShape = nextInputShape;
        outputShape = nextOutputShape;
        modelCapabilities = pack.capabilities;
        modelCapabilities.runtimeBackend = std::move(selectedBackend);
        modelCapabilities.fallbackReason = std::move(selectedFallbackReason);
        modelCapabilities.runtimeSignature = selectedSignature;
        profiled = !config.profilePrefix.empty();
        modelId = pack.id;
        inputBuffer.reset();
        outputBuffer.reset();
        inputBytes = 0;
        outputBytes = 0;
    }

    Capabilities getCapabilities() const {
        std::lock_guard lock(mutex);
        return modelCapabilities;
    }

    std::future<Result> submit(Request request) {
        std::lock_guard lock(mutex);
        if (!session.value) throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Load a verified restoration model before processing");
        if (jobs.size() >= config.maxQueuedRequests) throw inference::InferenceError(inference::InferenceErrorKind::busy,"Restoration queue is full");
        Job job{std::move(request),{}};
        auto future = job.promise.get_future();
        jobs.push_back(std::move(job));
        ready.notify_one();
        return future;
    }

    std::filesystem::path finishProfile() {
        std::lock_guard lock(mutex);
        if (active || !jobs.empty()) throw inference::InferenceError(inference::InferenceErrorKind::busy,"Cannot finish profiling while restoration is active");
        if (!session.value || !profiled) throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"No active restoration profile");
        OrtAllocator* allocator = nullptr;
        inference::check(api,api.GetAllocatorWithDefaultOptions(&allocator));
        char* name = nullptr;
        inference::check(api,api.SessionEndProfiling(session.value,allocator,&name));
        const std::string utf8 = name ? name : "";
        const auto path = std::filesystem::path(std::u8string(utf8.begin(),utf8.end()));
        allocator->Free(allocator,name);
        profiled = false;
        return path;
    }

private:
    Result execute(Request& request,std::stop_token shutdown) {
        const auto started = std::chrono::steady_clock::now();
        auto cancel = [&] {
            if (request.stop.stop_requested() || shutdown.stop_requested())
                throw inference::InferenceError(inference::InferenceErrorKind::cancelled,"Video restoration cancelled");
        };
        cancel();
        validateRequest(modelCapabilities,request);
        const auto& source = request.context[request.centerIndex];
        const bool temporalAdapter = supportsOnnxCudaTemporalAdapter(modelCapabilities);
        if (temporalAdapter && (!request.denoiseSigma255 || !std::isfinite(*request.denoiseSigma255) ||
            *request.denoiseSigma255 < 0.0F || *request.denoiseSigma255 > 55.0F ||
            (*request.denoiseSigma255 > 0.0F && *request.denoiseSigma255 < 5.0F)))
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"FastDVDnet needs explicit noise sigma 5..55, or zero to bypass denoising");
        if (temporalAdapter && *request.denoiseSigma255 == 0.0F)
            return Result{source,std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
        std::vector<media::RifeSdrFrame> preparedFrames;
        preparedFrames.reserve(temporalAdapter ? request.context.size() : 1);
        preparedFrames.push_back(media::prepareRifeSdr(source,config.hostFloatBufferLimit,request.stop));
        const auto singleFrameBytes = preparedFrames.front().pixels.size() * sizeof(float);
        const auto frameCount = temporalAdapter ? request.context.size() : 1;
        if (singleFrameBytes > config.hostFloatBufferLimit / (frameCount + 1))
            throw inference::InferenceError(inference::InferenceErrorKind::outOfMemory,"Restoration temporal context and output exceed the host float buffer budget");
        if (temporalAdapter) {
            // Keep one prepared tensor per real frame. Boundary repetitions are
            // packed directly into tiles and do not allocate invented frames.
            for (std::size_t index = 0; index < request.context.size(); ++index) {
                if (index == request.centerIndex) continue;
                preparedFrames.push_back(media::prepareRifeSdr(request.context[index],config.hostFloatBufferLimit,request.stop));
            }
            std::rotate(preparedFrames.begin(),preparedFrames.begin() + 1,preparedFrames.begin() + static_cast<std::ptrdiff_t>(request.centerIndex + 1));
        }
        const auto& prepared = preparedFrames[temporalAdapter ? request.centerIndex : 0];
        const std::size_t inputChannels = temporalAdapter ? 16 : 3;
        const auto scale = modelCapabilities.scaleNumerator;
        if (prepared.paddedWidth > std::numeric_limits<int>::max() / scale || prepared.paddedHeight > std::numeric_limits<int>::max() / scale ||
            source.width > std::numeric_limits<int>::max() / scale || source.height > std::numeric_limits<int>::max() / scale)
            throw inference::InferenceError(inference::InferenceErrorKind::outOfMemory,"Restoration output dimensions overflow");
        const int paddedOutputWidth = prepared.paddedWidth * scale;
        const int paddedOutputHeight = prepared.paddedHeight * scale;
        const int outputWidth = source.width * scale;
        const int outputHeight = source.height * scale;
        const auto outBytes = tensorBytes(paddedOutputWidth,paddedOutputHeight,config.hostFloatBufferLimit,"Restoration output");
        const auto inputBytesFull = prepared.pixels.size() * sizeof(float);
        if (inputBytesFull != tensorBytes(prepared.paddedWidth,prepared.paddedHeight,config.hostFloatBufferLimit,"Restoration input"))
            throw inference::InferenceError(inference::InferenceErrorKind::invalidRequest,"Restoration preprocessor returned a mismatched tensor size");
        cancel();
        auto context = device->activate();
        media::RifeSdrFrame generated{outputWidth,outputHeight,paddedOutputWidth,paddedOutputHeight,source.color,
            std::vector<float>(outBytes / sizeof(float))};
        const auto fullInputPlane = static_cast<std::size_t>(prepared.paddedWidth) * static_cast<std::size_t>(prepared.paddedHeight);
        const auto fullOutputPlane = static_cast<std::size_t>(paddedOutputWidth) * static_cast<std::size_t>(paddedOutputHeight);
        const int minimumTile = modelCapabilities.adapterId == "nafnet-zoo-deblur-rgb-f32-nchw-v1" ? 384 : 64;
        const int coreSize = std::max(config.tileSize,minimumTile);
        const int overlap = minimumTile == 384 ? std::max(config.tileOverlap,192) : temporalAdapter ? std::max(config.tileOverlap,64) : config.tileOverlap;
        const auto coreStep = [&](int dimension) {
            if (minimumTile != 384) return coreSize;
            // Use the whole axis when it fits the tested profile. For longer
            // axes distribute cores evenly to avoid a tiny, context-poor tail.
            if (dimension <= coreSize + 2 * overlap) return dimension;
            const int segments = (dimension + coreSize - 1) / coreSize;
            return ((dimension + segments * 64 - 1) / (segments * 64)) * 64;
        };
        const int coreWidthStep = coreStep(prepared.paddedWidth);
        const int coreHeightStep = coreStep(prepared.paddedHeight);
        const int maxTileWidth = std::max(minimumTile,std::min(prepared.paddedWidth,coreSize + 2 * overlap));
        const int maxTileHeight = std::max(minimumTile,std::min(prepared.paddedHeight,coreSize + 2 * overlap));
        const auto maxInputBytes = tensorBytes(maxTileWidth,maxTileHeight,config.hostFloatBufferLimit,"Restoration tile input",inputChannels);
        const auto maxOutputBytes = tensorBytes(maxTileWidth * scale,maxTileHeight * scale,config.hostFloatBufferLimit,"Restoration tile output");
        if (temporalAdapter && singleFrameBytes * (frameCount + 1) + maxInputBytes + maxOutputBytes > config.hostFloatBufferLimit)
            throw inference::InferenceError(inference::InferenceErrorKind::outOfMemory,"Restoration temporal window and tile buffers exceed the host float budget");
        if (!inputBuffer || inputBytes < maxInputBytes) { inputBuffer = device->allocate(maxInputBytes,request.stop); inputBytes = maxInputBytes; }
        if (!outputBuffer || outputBytes < maxOutputBytes) { outputBuffer = device->allocate(maxOutputBytes,request.stop); outputBytes = maxOutputBytes; }

        for (int coreY = 0; coreY < prepared.paddedHeight; coreY += coreHeightStep) {
            const int coreHeight = std::min(coreHeightStep,prepared.paddedHeight - coreY);
            int tileY = std::max(0,coreY - overlap);
            int tileBottom = std::min(prepared.paddedHeight,coreY + coreHeight + overlap);
            if (tileBottom - tileY < minimumTile) {
                const int extendBefore = std::min(tileY,minimumTile - (tileBottom - tileY));
                tileY -= extendBefore;
                tileBottom = std::min(prepared.paddedHeight,tileY + minimumTile);
            }
            const int tileHeight = tileBottom - tileY;
            const int localCoreY = coreY - tileY;
            for (int coreX = 0; coreX < prepared.paddedWidth; coreX += coreWidthStep) {
                cancel();
                const int coreWidth = std::min(coreWidthStep,prepared.paddedWidth - coreX);
                int tileX = std::max(0,coreX - overlap);
                int tileRight = std::min(prepared.paddedWidth,coreX + coreWidth + overlap);
                if (tileRight - tileX < minimumTile) {
                    const int extendBefore = std::min(tileX,minimumTile - (tileRight - tileX));
                    tileX -= extendBefore;
                    tileRight = std::min(prepared.paddedWidth,tileX + minimumTile);
                }
                const int tileWidth = tileRight - tileX;
                const int localCoreX = coreX - tileX;
                const int runTileWidth = std::max(minimumTile,tileWidth);
                const int runTileHeight = std::max(minimumTile,tileHeight);
                const auto tilePlane = static_cast<std::size_t>(runTileWidth) * static_cast<std::size_t>(runTileHeight);
                const auto tileInputBytes = tilePlane * inputChannels * sizeof(float);
                const int outputTileWidth = runTileWidth * scale;
                const int outputTileHeight = runTileHeight * scale;
                const auto outputTilePlane = static_cast<std::size_t>(outputTileWidth) * static_cast<std::size_t>(outputTileHeight);
                const auto tileOutputBytes = outputTilePlane * 3 * sizeof(float);
                std::vector<float> tileInput(tilePlane * inputChannels);
                for (std::size_t channel = 0; channel < inputChannels; ++channel) {
                    if (temporalAdapter && channel == 15) {
                        std::fill_n(tileInput.begin() + static_cast<std::ptrdiff_t>(channel * tilePlane),tilePlane,*request.denoiseSigma255 / 255.0F);
                        continue;
                    }
                    const auto frameIndex = temporalAdapter ? static_cast<std::size_t>(std::clamp(
                        static_cast<int>(channel / 3) - static_cast<int>(request.missingFramesBefore),0,static_cast<int>(preparedFrames.size()) - 1)) : 0;
                    const auto& framePixels = preparedFrames[frameIndex].pixels;
                    const auto rgbChannel = channel % 3;
                    for (int row = 0; row < runTileHeight; ++row) {
                        const auto src = rgbChannel * fullInputPlane + static_cast<std::size_t>(tileY + std::min(row,tileHeight - 1)) * static_cast<std::size_t>(prepared.paddedWidth) + static_cast<std::size_t>(tileX);
                        const auto dst = channel * tilePlane + static_cast<std::size_t>(row) * static_cast<std::size_t>(runTileWidth);
                        std::copy_n(framePixels.begin() + static_cast<std::ptrdiff_t>(src),tileWidth,tileInput.begin() + static_cast<std::ptrdiff_t>(dst));
                        std::fill_n(tileInput.begin() + static_cast<std::ptrdiff_t>(dst + static_cast<std::size_t>(tileWidth)),runTileWidth - tileWidth,
                            framePixels[src + static_cast<std::size_t>(tileWidth - 1)]);
                    }
                }
                inputBuffer->upload(std::as_bytes(std::span(tileInput)),0,request.stop);
                const std::int64_t inputDimensions[] = {1,static_cast<std::int64_t>(inputChannels),runTileHeight,runTileWidth};
                const std::int64_t outputDimensions[] = {1,3,outputTileHeight,outputTileWidth};
                inference::Handle<OrtValue> input(api.ReleaseValue), output(api.ReleaseValue);
                inference::check(api,api.CreateTensorWithDataAsOrtValue(cudaMemory.value,reinterpret_cast<void*>(inputBuffer->address()),tileInputBytes,inputDimensions,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&input.value));
                inference::check(api,api.CreateTensorWithDataAsOrtValue(cudaMemory.value,reinterpret_cast<void*>(outputBuffer->address()),tileOutputBytes,outputDimensions,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&output.value));
                inference::Handle<OrtIoBinding> binding(api.ReleaseIoBinding);
                inference::check(api,api.CreateIoBinding(session.value,&binding.value));
                inference::check(api,api.BindInput(binding.value,inputName.c_str(),input.value));
                inference::check(api,api.BindOutput(binding.value,outputName.c_str(),output.value));
                inference::Handle<OrtRunOptions> options(api.ReleaseRunOptions);
                inference::check(api,api.CreateRunOptions(&options.value));
                auto terminate = [&] { if (auto* status = api.RunOptionsSetTerminate(options.value)) api.ReleaseStatus(status); };
                std::stop_callback requested(request.stop,terminate);
                std::stop_callback stopping(shutdown,terminate);
                try {
                    inference::check(api,api.RunWithBinding(session.value,options.value,binding.value),request.stop,shutdown);
                    inference::check(api,api.SynchronizeBoundOutputs(binding.value),request.stop,shutdown);
                    device->synchronize(request.stop);
                } catch (...) {
                    try { device->synchronize(); } catch (...) {}
                    throw;
                }
                cancel();
                std::vector<float> tileOutput(tileOutputBytes / sizeof(float));
                outputBuffer->download(std::as_writable_bytes(std::span(tileOutput)),0,request.stop);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    for (int row = 0; row < coreHeight * scale; ++row) {
                        const auto src = channel * outputTilePlane + static_cast<std::size_t>(localCoreY * scale + row) * static_cast<std::size_t>(outputTileWidth) + static_cast<std::size_t>(localCoreX * scale);
                        const auto dst = channel * fullOutputPlane + static_cast<std::size_t>(coreY * scale + row) * static_cast<std::size_t>(paddedOutputWidth) + static_cast<std::size_t>(coreX * scale);
                        const auto count = static_cast<std::size_t>(coreWidth * scale);
                        std::copy_n(tileOutput.begin() + static_cast<std::ptrdiff_t>(src),count,generated.pixels.begin() + static_cast<std::ptrdiff_t>(dst));
                    }
                }
            }
        }
        for (auto& value : generated.pixels) {
            if (!std::isfinite(value)) throw inference::InferenceError(inference::InferenceErrorKind::runtimeError,"Restoration model returned a non-finite pixel value");
            value = std::clamp(value,0.0F,1.0F);
        }
        auto outputFrame = media::restoreRifeSdr(generated,request.stop);
        outputFrame.index = source.index;
        outputFrame.timestamp = source.timestamp;
        outputFrame.duration = source.duration;
        outputFrame.timestampRecovered = source.timestampRecovered;
        Result result{std::move(outputFrame),std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
        validateResult(modelCapabilities,request,result);
        return result;
    }

    void work(std::stop_token stop) {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock,stop,[this] { return !jobs.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(jobs.front()); jobs.pop_front(); active = true;
            }
            try {
                auto result = execute(job.request,stop);
                { std::lock_guard lock(mutex); active = false; }
                job.promise.set_value(std::move(result));
            } catch (...) {
                auto exception = std::current_exception();
                try { std::rethrow_exception(exception); }
                catch (const gpu::GpuError& error) {
                    auto kind = inference::InferenceErrorKind::runtimeError;
                    if (error.kind() == gpu::GpuErrorKind::outOfMemory) kind = inference::InferenceErrorKind::outOfMemory;
                    else if (error.kind() == gpu::GpuErrorKind::cancelled) kind = inference::InferenceErrorKind::cancelled;
                    else if (error.kind() == gpu::GpuErrorKind::invalidRequest) kind = inference::InferenceErrorKind::invalidRequest;
                    else if (error.kind() == gpu::GpuErrorKind::unavailable) kind = inference::InferenceErrorKind::unavailable;
                    exception = std::make_exception_ptr(inference::InferenceError(kind,error.what()));
                } catch (...) {}
                { std::lock_guard lock(mutex); active = false; }
                job.promise.set_exception(exception);
            }
        }
        std::lock_guard lock(mutex);
        for (auto& job : jobs)
            job.promise.set_exception(std::make_exception_ptr(inference::InferenceError(inference::InferenceErrorKind::cancelled,"Restoration backend stopped before request execution")));
        jobs.clear();
    }

    std::shared_ptr<gpu::CudaDevice> device;
    OnnxCudaRestorationConfig config;
    std::shared_ptr<inference::Runtime> runtime;
    const OrtApi& api;
    inference::Handle<OrtSession> session;
    inference::Handle<OrtMemoryInfo> cudaMemory;
    std::shared_ptr<gpu::DeviceBuffer> inputBuffer;
    std::shared_ptr<gpu::DeviceBuffer> outputBuffer;
    std::size_t inputBytes = 0;
    std::size_t outputBytes = 0;
    std::string inputName;
    std::string outputName;
    std::array<std::int64_t,4> inputShape{};
    std::array<std::int64_t,4> outputShape{};
    Capabilities modelCapabilities{};
    std::string modelId;
    mutable std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Job> jobs;
    bool active = false;
    bool profiled = false;
    std::jthread worker;
};

OnnxCudaVideoRestorationBackend::OnnxCudaVideoRestorationBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaRestorationConfig config)
    : impl_(std::make_unique<Impl>(std::move(device),std::move(config))) {}
OnnxCudaVideoRestorationBackend::~OnnxCudaVideoRestorationBackend() = default;
Capabilities OnnxCudaVideoRestorationBackend::capabilities() const { return impl_->getCapabilities(); }
void OnnxCudaVideoRestorationBackend::loadModel(const ModelPack& model) { impl_->load(model); }
std::future<Result> OnnxCudaVideoRestorationBackend::restore(Request request) { return impl_->submit(std::move(request)); }
std::filesystem::path OnnxCudaVideoRestorationBackend::finishProfiling() { return impl_->finishProfile(); }

} // namespace rifelove::restoration
#endif

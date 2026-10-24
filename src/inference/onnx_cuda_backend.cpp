#include "inference/onnx_cuda_backend.hpp"

// The pinned Microsoft header spells the Windows calling convention _stdcall.
// GCC in strict C++20 mode exposes __stdcall instead. Both use the x64 C ABI.
#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
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
private:
    OrtEnv* environment_{};
    HMODULE module_{};
    std::vector<HMODULE> dependencyModules_;
    void cleanup() noexcept {
        if (environment_) { api->ReleaseEnv(environment_); environment_ = nullptr; }
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        for (auto iterator = dependencyModules_.rbegin(); iterator != dependencyModules_.rend(); ++iterator) FreeLibrary(*iterator);
        dependencyModules_.clear();
    }
};
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
        const std::array<std::int64_t, 4> expected = index == 2 ? std::array<std::int64_t, 4>{1,1,1,1} : std::array<std::int64_t, 4>{1,3,-1,-1};
        if (dimensions != expected) throw InferenceError(InferenceErrorKind::invalidRequest, "RIFE shape contract requires dynamic spatial axes and scalar timestep");
    }
}
}

class OnnxCudaBackend::Impl final {
public:
    struct Job { InterpolationRequest request; std::promise<InterpolationResult> promise; };
    Impl(std::shared_ptr<gpu::CudaDevice> selected, OnnxCudaConfig settings)
        : device(std::move(selected)), config(std::move(settings)), runtime(processRuntime(config.runtimeDirectory)), api(*runtime->api),
          session(api.ReleaseSession), cudaMemory(api.ReleaseMemoryInfo) {
        if (!device || !config.arenaBudgetBytes || !config.maxQueuedRequests || config.maxQueuedRequests > 64)
            throw InferenceError(InferenceErrorKind::invalidRequest, "Invalid CUDA backend device, budget or queue limit");
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
        Handle<OrtSessionOptions> options(api.ReleaseSessionOptions);
        check(api, api.CreateSessionOptions(&options.value));
        check(api, api.SetSessionExecutionMode(options.value, ORT_SEQUENTIAL));
        check(api, api.SetIntraOpNumThreads(options.value, 2));
        check(api, api.SetInterOpNumThreads(options.value, 1));
        check(api, api.SetSessionGraphOptimizationLevel(options.value, ORT_ENABLE_EXTENDED));
        if (!config.profilePrefix.empty()) check(api, api.EnableProfiling(options.value, config.profilePrefix.c_str()));
        Handle<OrtCUDAProviderOptionsV2> provider(api.ReleaseCUDAProviderOptions);
        check(api, api.CreateCUDAProviderOptions(&provider.value));
        const std::string ordinal = std::to_string(device->info().ordinal), budget = std::to_string(config.arenaBudgetBytes);
        const char* keys[] = {"device_id", "gpu_mem_limit", "arena_extend_strategy", "cudnn_conv_algo_search", "cudnn_conv_use_max_workspace", "use_tf32", "do_copy_in_default_stream"};
        // Use the documented implicit-precomputed-GEMM baseline. Heuristic
        // selection showed a gate failure on a repeated native run; faster
        // algorithms need their own numerical qualification before use.
        const char* values[] = {ordinal.c_str(), budget.c_str(), "kSameAsRequested", "DEFAULT", "0", "0", "1"};
        check(api, api.UpdateCUDAProviderOptions(provider.value, keys, values, std::size(keys)));
        check(api, api.UpdateCUDAProviderOptionsWithValue(provider.value, "user_compute_stream", reinterpret_cast<void*>(device->streamAddress())));
        // CUDA provider creation failure propagates. There is no CPU-session retry.
        try { check(api, api.SessionOptionsAppendExecutionProvider_CUDA_V2(options.value, provider.value)); }
        catch (const InferenceError& error) { throw InferenceError(InferenceErrorKind::unavailable, "CUDA provider could not initialize: " + std::string(error.what())); }
        Handle<OrtSession> next(api.ReleaseSession);
        check(api, api.CreateSessionFromArray(runtime->environment(), model.graph.data(), model.graph.size(), options.value, &next.value));
        validateContract(api, next.value);
        session = std::move(next);
        modelId = model.id;
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
        return {"onnxruntime_cuda", modelId, 64, true, false};
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
    std::jthread worker;
};
OnnxCudaBackend::OnnxCudaBackend(std::shared_ptr<gpu::CudaDevice> device, OnnxCudaConfig config) : impl_(std::make_unique<Impl>(std::move(device), std::move(config))) {}
OnnxCudaBackend::~OnnxCudaBackend() = default;
ModelCapabilities OnnxCudaBackend::capabilities() const { return impl_->capabilities(); }
void OnnxCudaBackend::loadModel(const ModelPack& model) { impl_->load(model); }
std::future<InterpolationResult> OnnxCudaBackend::interpolate(InterpolationRequest request) { return impl_->submit(std::move(request)); }
std::filesystem::path OnnxCudaBackend::finishProfiling() { return impl_->finishProfile(); }
} // namespace rifelove::inference

#include "gpu/cuda_device.hpp"

#include <algorithm>
#include <atomic>
#include <bit>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define RIFELOVE_CUDA_CALL __stdcall
#else
#define RIFELOVE_CUDA_CALL
#endif

namespace rifelove::gpu {
GpuError::GpuError(GpuErrorKind kind, const std::string& message, std::optional<int> code)
    : std::runtime_error(message), kind_(kind), nativeCode_(code) {}

namespace {
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw GpuError(GpuErrorKind::cancelled, "GPU operation cancelled"); }
void checkRange(std::size_t size, std::size_t offset, std::size_t bytes) {
    if (offset > size || bytes > size - offset) throw GpuError(GpuErrorKind::invalidRequest, "GPU transfer exceeds the owned buffer");
}
}

namespace detail {
class CudaState final {
public:
    using Init = int(RIFELOVE_CUDA_CALL*)(unsigned);
    using GetVersion = int(RIFELOVE_CUDA_CALL*)(int*);
    using DeviceGet = int(RIFELOVE_CUDA_CALL*)(int*, int);
    using GetName = int(RIFELOVE_CUDA_CALL*)(char*, int, int);
    using GetAttribute = int(RIFELOVE_CUDA_CALL*)(int*, int, int);
    using TotalMemory = int(RIFELOVE_CUDA_CALL*)(std::size_t*, int);
    using GetBusId = int(RIFELOVE_CUDA_CALL*)(char*, int, int);
    using PrimaryRetain = int(RIFELOVE_CUDA_CALL*)(void**, int);
    using PrimaryRelease = int(RIFELOVE_CUDA_CALL*)(int);
    using PushContext = int(RIFELOVE_CUDA_CALL*)(void*);
    using PopContext = int(RIFELOVE_CUDA_CALL*)(void**);
    using StreamCreate = int(RIFELOVE_CUDA_CALL*)(void**, unsigned);
    using StreamOperation = int(RIFELOVE_CUDA_CALL*)(void*);
    using Allocate = int(RIFELOVE_CUDA_CALL*)(std::uint64_t*, std::size_t);
    using Free = int(RIFELOVE_CUDA_CALL*)(std::uint64_t);
    using HostToDevice = int(RIFELOVE_CUDA_CALL*)(std::uint64_t, const void*, std::size_t);
    using DeviceToHost = int(RIFELOVE_CUDA_CALL*)(void*, std::uint64_t, std::size_t);
    using DeviceToDevice = int(RIFELOVE_CUDA_CALL*)(std::uint64_t, std::uint64_t, std::size_t);
    using GetMemory = int(RIFELOVE_CUDA_CALL*)(std::size_t*, std::size_t*);
    using ErrorText = int(RIFELOVE_CUDA_CALL*)(int, const char**);

    CudaState(int ordinal, std::size_t requestedBudget) {
        if (ordinal < 0) throw GpuError(GpuErrorKind::invalidRequest, "CUDA device ordinal cannot be negative");
#ifdef _WIN32
        library_ = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) throw GpuError(GpuErrorKind::unavailable, "The NVIDIA CUDA driver is not installed or could not be loaded");
        try {
            const auto init = function<Init>("cuInit");
            const auto version = function<GetVersion>("cuDriverGetVersion");
            const auto deviceGet = function<DeviceGet>("cuDeviceGet");
            const auto name = function<GetName>("cuDeviceGetName");
            const auto attribute = function<GetAttribute>("cuDeviceGetAttribute");
            const auto total = function<TotalMemory>("cuDeviceTotalMem_v2");
            const auto bus = function<GetBusId>("cuDeviceGetPCIBusId");
            retain = function<PrimaryRetain>("cuDevicePrimaryCtxRetain");
            release = function<PrimaryRelease>("cuDevicePrimaryCtxRelease_v2");
            push = function<PushContext>("cuCtxPushCurrent_v2");
            pop = function<PopContext>("cuCtxPopCurrent_v2");
            createStream = function<StreamCreate>("cuStreamCreate");
            destroyStream = function<StreamOperation>("cuStreamDestroy_v2");
            syncStream = function<StreamOperation>("cuStreamSynchronize");
            allocate = function<Allocate>("cuMemAlloc_v2");
            free = function<Free>("cuMemFree_v2");
            upload = function<HostToDevice>("cuMemcpyHtoD_v2");
            download = function<DeviceToHost>("cuMemcpyDtoH_v2");
            copy = function<DeviceToDevice>("cuMemcpyDtoD_v2");
            getMemory = function<GetMemory>("cuMemGetInfo_v2");
            errorText = function<ErrorText>("cuGetErrorString");
            check(init(0), "initialize CUDA driver");
            check(deviceGet(&device_, ordinal), "select CUDA device");
            info.ordinal = ordinal;
            char label[256]{};
            char pci[64]{};
            check(name(label, sizeof(label), device_), "read CUDA device name");
            check(bus(pci, sizeof(pci), device_), "read CUDA PCI identity");
            info.name = label;
            info.pciBusId = pci;
            check(version(&info.driverCudaApiVersion), "query CUDA driver API version");
            // Stable CUDA Driver API attribute values from CUdevice_attribute.
            check(attribute(&info.computeMajor, 75, device_), "query CUDA compute major");
            check(attribute(&info.computeMinor, 76, device_), "query CUDA compute minor");
            check(total(&info.totalBytes, device_), "query CUDA device memory");
            check(retain(&context, device_), "retain CUDA primary context");
            check(push(context), "activate CUDA primary context");
            bool pushed = true;
            try {
                check(createStream(&stream, 1), "create nonblocking CUDA stream");
                std::size_t available = 0, totalBytes = 0;
                check(getMemory(&available, &totalBytes), "query CUDA free memory");
                const auto defaultBudget = std::min<std::size_t>(available / 4, 1024ULL * 1024 * 1024);
                budget = requestedBudget ? std::min(requestedBudget, available) : defaultBudget;
                if (budget == 0) throw GpuError(GpuErrorKind::outOfMemory, "No CUDA buffer memory is available");
                void* ignored = nullptr;
                check(pop(&ignored), "restore previous CUDA context");
                pushed = false;
            } catch (...) {
                if (pushed) { void* ignored = nullptr; pop(&ignored); }
                throw;
            }
        } catch (...) { cleanup(); throw; }
#else
        (void)requestedBudget;
        throw GpuError(GpuErrorKind::unavailable, "This CUDA driver loader currently supports Windows x64 only");
#endif
    }
    ~CudaState() { cleanup(); }
    CudaState(const CudaState&) = delete;
    CudaState& operator=(const CudaState&) = delete;
    void check(int code, const std::string& operation) const {
        if (!code) return;
        const char* text = nullptr;
        if (errorText) errorText(code, &text);
        const auto kind = code == 2 ? GpuErrorKind::outOfMemory : (code == 100 || code == 803 ? GpuErrorKind::unavailable : GpuErrorKind::driverError);
        throw GpuError(kind, operation + ": " + (text ? text : "CUDA error " + std::to_string(code)), code);
    }
    DeviceInfo info{};
    std::size_t budget = 0;
    std::atomic<std::size_t> allocated{0};
    void* context{};
    void* stream{};
    PrimaryRetain retain{};
    PrimaryRelease release{};
    PushContext push{};
    PopContext pop{};
    StreamCreate createStream{};
    StreamOperation destroyStream{};
    StreamOperation syncStream{};
    Allocate allocate{};
    Free free{};
    HostToDevice upload{};
    DeviceToHost download{};
    DeviceToDevice copy{};
    GetMemory getMemory{};
    ErrorText errorText{};
private:
#ifdef _WIN32
    template<class T> T function(const char* name) {
        const auto address = GetProcAddress(library_, name);
        if (!address) throw GpuError(GpuErrorKind::unavailable, "NVIDIA driver is missing required entry point: " + std::string(name));
        return std::bit_cast<T>(address);
    }
    HMODULE library_{};
#endif
    int device_ = 0;
    void cleanup() noexcept {
        if (context && release) {
            if (push && pop && push(context) == 0) {
                if (stream && destroyStream) destroyStream(stream);
                void* ignored = nullptr;
                pop(&ignored);
            }
            release(device_);
            context = nullptr;
        }
#ifdef _WIN32
        if (library_) { FreeLibrary(library_); library_ = nullptr; }
#endif
    }
};

class ContextGuard final {
public:
    explicit ContextGuard(CudaState& state) : state_(state) { state_.check(state_.push(state_.context), "activate CUDA context"); }
    ~ContextGuard() { void* ignored = nullptr; state_.pop(&ignored); }
    ContextGuard(const ContextGuard&) = delete;
    ContextGuard& operator=(const ContextGuard&) = delete;
private:
    CudaState& state_;
};
} // namespace detail

ScopedCudaContext::ScopedCudaContext(std::shared_ptr<detail::CudaState> state) : state_(std::move(state)) {
    state_->check(state_->push(state_->context), "activate CUDA context");
}
ScopedCudaContext::~ScopedCudaContext() { void* ignored = nullptr; state_->pop(&ignored); }

DeviceBuffer::DeviceBuffer(std::shared_ptr<detail::CudaState> state) : state_(std::move(state)) {}
DeviceBuffer::~DeviceBuffer() {
    if (address_) {
        try { detail::ContextGuard guard(*state_); state_->free(address_); } catch (...) {}
        state_->allocated.fetch_sub(size_);
    }
}
std::uintptr_t DeviceBuffer::address() const noexcept { return static_cast<std::uintptr_t>(address_); }
std::size_t DeviceBuffer::size() const noexcept { return size_; }
int DeviceBuffer::deviceOrdinal() const noexcept { return state_->info.ordinal; }
void DeviceBuffer::upload(std::span<const std::byte> source, std::size_t offset, std::stop_token stop) {
    cancelled(stop);
    checkRange(size_, offset, source.size());
    if (source.empty()) return;
    detail::ContextGuard guard(*state_);
    state_->check(state_->upload(address_ + offset, source.data(), source.size()), "upload CUDA buffer");
    // CUDA's host-return semantics depend on memory type. Complete the default
    // stream transfer before a nonblocking inference stream consumes this data.
    state_->check(state_->syncStream(nullptr), "complete CUDA upload");
    cancelled(stop);
}
void DeviceBuffer::download(std::span<std::byte> destination, std::size_t offset, std::stop_token stop) const {
    cancelled(stop);
    checkRange(size_, offset, destination.size());
    if (destination.empty()) return;
    detail::ContextGuard guard(*state_);
    state_->check(state_->download(destination.data(), address_ + offset, destination.size()), "download CUDA buffer");
    state_->check(state_->syncStream(nullptr), "complete CUDA download");
    cancelled(stop);
}
CudaDevice::CudaDevice(int ordinal, std::size_t budget) : state_(std::make_shared<detail::CudaState>(ordinal, budget)) {}
const DeviceInfo& CudaDevice::info() const noexcept { return state_->info; }
MemoryInfo CudaDevice::memory() const {
    detail::ContextGuard guard(*state_);
    MemoryInfo result{};
    state_->check(state_->getMemory(&result.freeBytes, &result.totalBytes), "query CUDA memory");
    return result;
}
std::size_t CudaDevice::bufferBudget() const noexcept { return state_->budget; }
std::size_t CudaDevice::allocatedBytes() const noexcept { return state_->allocated.load(); }
std::uintptr_t CudaDevice::streamAddress() const noexcept { return reinterpret_cast<std::uintptr_t>(state_->stream); }
ScopedCudaContext CudaDevice::activate() const { return ScopedCudaContext(state_); }
std::shared_ptr<DeviceBuffer> CudaDevice::allocate(std::size_t bytes, std::stop_token stop) {
    cancelled(stop);
    if (bytes == 0) throw GpuError(GpuErrorKind::invalidRequest, "CUDA allocations must have a positive size");
    auto result = std::shared_ptr<DeviceBuffer>(new DeviceBuffer(state_));
    auto current = state_->allocated.load();
    for (;;) {
        if (current > state_->budget || bytes > state_->budget - current) throw GpuError(GpuErrorKind::outOfMemory, "CUDA buffer budget would be exceeded");
        if (state_->allocated.compare_exchange_weak(current, current + bytes)) break;
    }
    std::uint64_t address = 0;
    try {
        detail::ContextGuard guard(*state_);
        state_->check(state_->allocate(&address, bytes), "allocate CUDA device buffer");
    } catch (...) { state_->allocated.fetch_sub(bytes); throw; }
    result->address_ = address;
    result->size_ = bytes;
    cancelled(stop);
    return result;
}
void CudaDevice::synchronize(std::stop_token stop) const {
    cancelled(stop);
    detail::ContextGuard guard(*state_);
    state_->check(state_->syncStream(state_->stream), "synchronize CUDA stream");
    cancelled(stop);
}
std::shared_ptr<DeviceBuffer> CudaDevice::reclaim(std::shared_ptr<const DeviceBuffer> buffer) const {
    if (!buffer || buffer.use_count() != 1 || buffer->deviceOrdinal() != info().ordinal)
        throw GpuError(GpuErrorKind::invalidRequest, "Reclaim requires the sole reference to a completed buffer on this device");
    return std::const_pointer_cast<DeviceBuffer>(std::move(buffer));
}
void CudaDevice::copy(const DeviceBuffer& source, DeviceBuffer& destination, std::size_t bytes, std::stop_token stop) const {
    cancelled(stop);
    if (source.deviceOrdinal() != info().ordinal || destination.deviceOrdinal() != info().ordinal)
        throw GpuError(GpuErrorKind::invalidRequest, "CUDA device copy requires buffers on the same device");
    checkRange(source.size(), 0, bytes);
    checkRange(destination.size(), 0, bytes);
    if (!bytes) return;
    detail::ContextGuard guard(*state_);
    state_->check(state_->copy(destination.address_, source.address_, bytes), "copy CUDA device buffer");
    state_->check(state_->syncStream(nullptr), "complete CUDA device copy");
    cancelled(stop);
}

DeviceBufferPool::DeviceBufferPool(CudaDevice& device, std::size_t maxCachedBuffers)
    : device_(&device), maxCachedBuffers_(maxCachedBuffers) {}

std::shared_ptr<DeviceBuffer> DeviceBufferPool::acquire(std::size_t bytes, std::stop_token stop) {
    cancelled(stop);
    if (bytes == 0) throw GpuError(GpuErrorKind::invalidRequest, "CUDA pool allocations must have a positive size");
    for (const auto& buffer : buffers_) {
        if (buffer->size() == bytes && buffer.use_count() == 1) {
            ++reuses_;
            return buffer;
        }
    }

    // If the cache is full, replace an idle differently-sized allocation. A
    // still-referenced buffer remains pinned until its inference future ends.
    if (buffers_.size() >= maxCachedBuffers_) {
        const auto idle = std::find_if(buffers_.begin(), buffers_.end(), [](const auto& buffer) {
            return buffer.use_count() == 1;
        });
        if (idle != buffers_.end()) buffers_.erase(idle);
    }

    auto storage = [&] { return device_->allocate(bytes, stop); };
    std::shared_ptr<DeviceBuffer> result;
    try {
        result = storage();
    } catch (const GpuError& error) {
        if (error.kind() != GpuErrorKind::outOfMemory) throw;
        const auto previousSize = buffers_.size();
        buffers_.erase(std::remove_if(buffers_.begin(), buffers_.end(), [](const auto& buffer) {
            return buffer.use_count() == 1;
        }), buffers_.end());
        if (buffers_.size() == previousSize) throw;
        result = storage();
    }

    ++allocations_;
    if (buffers_.size() < maxCachedBuffers_) buffers_.push_back(result);
    return result;
}

void DeviceBufferPool::clear() noexcept { buffers_.clear(); }
} // namespace rifelove::gpu

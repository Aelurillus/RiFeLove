#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace rifelove::gpu {

enum class GpuErrorKind { unavailable, invalidRequest, outOfMemory, cancelled, driverError };
class GpuError : public std::runtime_error {
public:
    GpuError(GpuErrorKind kind, const std::string& message, std::optional<int> nativeCode = {});
    [[nodiscard]] GpuErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::optional<int> nativeCode() const noexcept { return nativeCode_; }
private:
    GpuErrorKind kind_;
    std::optional<int> nativeCode_;
};

struct DeviceInfo {
    int ordinal;
    std::string name;
    std::string pciBusId;
    int computeMajor;
    int computeMinor;
    int driverCudaApiVersion; // Maximum driver API, not an installed CUDA Toolkit version.
    std::size_t totalBytes;
};
struct MemoryInfo { std::size_t freeBytes; std::size_t totalBytes; };
namespace detail { class CudaState; }

class ScopedCudaContext final {
public:
    ~ScopedCudaContext();
    ScopedCudaContext(const ScopedCudaContext&) = delete;
    ScopedCudaContext& operator=(const ScopedCudaContext&) = delete;
private:
    friend class CudaDevice;
    explicit ScopedCudaContext(std::shared_ptr<detail::CudaState> state);
    std::shared_ptr<detail::CudaState> state_;
};

class DeviceBuffer final {
public:
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    [[nodiscard]] std::uintptr_t address() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] int deviceOrdinal() const noexcept;
    void upload(std::span<const std::byte> source, std::size_t offset = 0, std::stop_token stop = {});
    void download(std::span<std::byte> destination, std::size_t offset = 0, std::stop_token stop = {}) const;
private:
    friend class CudaDevice;
    explicit DeviceBuffer(std::shared_ptr<detail::CudaState> state);
    std::shared_ptr<detail::CudaState> state_;
    std::uint64_t address_ = 0;
    std::size_t size_ = 0;
};

class CudaDevice final {
public:
    // Uses only the installed driver; it does not require nvcc, a Toolkit, or Python.
    explicit CudaDevice(int ordinal = 0, std::size_t bufferBudgetBytes = 0);
    [[nodiscard]] const DeviceInfo& info() const noexcept;
    [[nodiscard]] MemoryInfo memory() const;
    [[nodiscard]] std::size_t bufferBudget() const noexcept;
    [[nodiscard]] std::size_t allocatedBytes() const noexcept;
    [[nodiscard]] std::uintptr_t streamAddress() const noexcept;
    [[nodiscard]] ScopedCudaContext activate() const;
    [[nodiscard]] std::shared_ptr<DeviceBuffer> allocate(std::size_t bytes, std::stop_token stop = {});
    // Reclaim an immutable result only after its future has completed and all
    // other references are gone. This enables exclusive output-buffer reuse.
    [[nodiscard]] std::shared_ptr<DeviceBuffer> reclaim(std::shared_ptr<const DeviceBuffer> buffer) const;
    void synchronize(std::stop_token stop = {}) const;
    void copy(const DeviceBuffer& source, DeviceBuffer& destination, std::size_t bytes, std::stop_token stop = {}) const;
private:
    std::shared_ptr<detail::CudaState> state_;
};

// Reuses completed, exclusive frame buffers while keeping the number of cached
// allocations bounded. Active buffers are never overwritten or evicted. The
// pool is intended for one pipeline thread and is not internally synchronized.
class DeviceBufferPool final {
public:
    explicit DeviceBufferPool(CudaDevice& device, std::size_t maxCachedBuffers = 2);
    DeviceBufferPool(const DeviceBufferPool&) = delete;
    DeviceBufferPool& operator=(const DeviceBufferPool&) = delete;
    [[nodiscard]] std::shared_ptr<DeviceBuffer> acquire(std::size_t bytes, std::stop_token stop = {});
    void clear() noexcept;
    [[nodiscard]] std::size_t allocationCount() const noexcept { return allocations_; }
    [[nodiscard]] std::size_t reuseCount() const noexcept { return reuses_; }
    [[nodiscard]] std::size_t cachedBufferCount() const noexcept { return buffers_.size(); }
private:
    CudaDevice* device_;
    std::size_t maxCachedBuffers_;
    std::vector<std::shared_ptr<DeviceBuffer>> buffers_;
    std::size_t allocations_ = 0;
    std::size_t reuses_ = 0;
};

} // namespace rifelove::gpu

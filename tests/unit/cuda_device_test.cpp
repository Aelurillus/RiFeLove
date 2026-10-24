#include "gpu/cuda_device.hpp"
#include <gtest/gtest.h>

#include <vector>

using rifelove::gpu::CudaDevice;
using rifelove::gpu::DeviceBufferPool;
using rifelove::gpu::GpuError;
using rifelove::gpu::GpuErrorKind;

TEST(CudaDevice, RejectsNegativeDeviceBeforeLoadingDriver) {
    try { CudaDevice invalid(-1); FAIL() << "negative device was accepted"; }
    catch (const GpuError& error) { EXPECT_EQ(error.kind(), GpuErrorKind::invalidRequest); }
}

class CudaHardware : public ::testing::Test {
protected:
    void SetUp() override {
        try { device = std::make_unique<CudaDevice>(0, 8192); }
        catch (const GpuError& error) {
            if (error.kind() == GpuErrorKind::unavailable) GTEST_SKIP() << error.what();
            throw;
        }
    }
    std::unique_ptr<CudaDevice> device;
};

TEST_F(CudaHardware, ReportsActualDeviceAndMemory) {
    EXPECT_FALSE(device->info().name.empty());
    EXPECT_FALSE(device->info().pciBusId.empty());
    EXPECT_GT(device->info().computeMajor, 0);
    EXPECT_GT(device->info().driverCudaApiVersion, 0);
    const auto memory = device->memory();
    EXPECT_LE(memory.freeBytes, memory.totalBytes);
    EXPECT_GT(memory.totalBytes, 0U);
    EXPECT_NE(device->streamAddress(), 0U);
}

TEST_F(CudaHardware, EnforcesBudgetAndReturnsReservationsAfterFree) {
    auto first = device->allocate(4096);
    auto second = device->allocate(4096);
    EXPECT_EQ(device->allocatedBytes(), 8192U);
    try { (void)device->allocate(1); FAIL() << "buffer budget was not enforced"; }
    catch (const GpuError& error) { EXPECT_EQ(error.kind(), GpuErrorKind::outOfMemory); EXPECT_FALSE(error.nativeCode()); }
    first.reset();
    EXPECT_EQ(device->allocatedBytes(), 4096U);
    auto third = device->allocate(1024);
    EXPECT_EQ(device->allocatedBytes(), 5120U);
    EXPECT_THROW((void)device->allocate(0), GpuError);
}

TEST_F(CudaHardware, UploadCopyDownloadAndBoundsAreCorrect) {
    auto source = device->allocate(4096);
    auto destination = device->allocate(4096);
    std::vector<std::uint8_t> input(4096), output(4096);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(i % 251);
    source->upload(std::as_bytes(std::span(input)));
    device->copy(*source, *destination, input.size());
    destination->download(std::as_writable_bytes(std::span(output)));
    EXPECT_EQ(input, output);
    EXPECT_THROW(source->upload(std::as_bytes(std::span(input)), 1), GpuError);
    EXPECT_THROW(destination->download(std::as_writable_bytes(std::span(output)), 1), GpuError);
    std::stop_source stop;
    stop.request_stop();
    try { source->upload(std::as_bytes(std::span(input)), 0, stop.get_token()); FAIL() << "cancelled upload ran"; }
    catch (const GpuError& error) { EXPECT_EQ(error.kind(), GpuErrorKind::cancelled); }
    device->synchronize();
}

TEST_F(CudaHardware, BuffersKeepTheirContextAliveAfterDeviceWrapperCloses) {
    auto buffer = device->allocate(128);
    std::vector<float> input(32, 0.425F), output(32);
    buffer->upload(std::as_bytes(std::span(input)));
    device.reset();
    buffer->download(std::as_writable_bytes(std::span(output)));
    EXPECT_EQ(input, output);
}
TEST_F(CudaHardware, ReclaimRequiresExclusiveCompletedStorageAndPreservesItsAllocation) {
    auto mutableBuffer = device->allocate(1024);
    const auto address = mutableBuffer->address();
    std::shared_ptr<const rifelove::gpu::DeviceBuffer> readOnly = mutableBuffer;
    EXPECT_THROW((void)device->reclaim(readOnly),GpuError);
    mutableBuffer.reset();
    EXPECT_THROW((void)device->reclaim(readOnly),GpuError);
    auto reclaimed = device->reclaim(std::move(readOnly));
    EXPECT_EQ(reclaimed->address(),address); EXPECT_EQ(device->allocatedBytes(),1024U);
    std::vector<float> input(256,0.75F), output(256);
    reclaimed->upload(std::as_bytes(std::span(input)));
    reclaimed->download(std::as_writable_bytes(std::span(output)));
    EXPECT_EQ(input,output);
}

TEST_F(CudaHardware, BufferPoolReusesOnlyCompletedSizeMatchesAndReleasesIdleMemory) {
    DeviceBufferPool pool(*device,2);
    auto first = pool.acquire(4096);
    const auto firstAddress = first->address();
    auto second = pool.acquire(4096);
    EXPECT_NE(second->address(),firstAddress);
    EXPECT_EQ(pool.allocationCount(),2U);
    EXPECT_EQ(pool.reuseCount(),0U);
    EXPECT_EQ(pool.cachedBufferCount(),2U);
    EXPECT_EQ(device->allocatedBytes(),8192U);

    first.reset();
    auto reused = pool.acquire(4096);
    EXPECT_EQ(reused->address(),firstAddress);
    EXPECT_EQ(pool.reuseCount(),1U);
    reused.reset();
    second.reset();
    pool.clear();
    EXPECT_EQ(device->allocatedBytes(),0U);

    auto idleA = pool.acquire(4096);
    auto idleB = pool.acquire(4096);
    idleA.reset();
    idleB.reset();
    auto smaller = pool.acquire(1);
    EXPECT_EQ(smaller->size(),1U);
    EXPECT_EQ(pool.cachedBufferCount(),2U);
    EXPECT_EQ(device->allocatedBytes(),4097U);
    smaller.reset();
    pool.clear();
    EXPECT_EQ(device->allocatedBytes(),0U);
}

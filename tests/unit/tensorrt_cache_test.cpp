#include "inference/tensorrt_cache.hpp"
#include <gtest/gtest.h>

using namespace rifelove::inference;

TEST(TensorRtCache, PrefixIsStableAndBindsModelRuntimeDeviceAndBuildOptions) {
    ModelPack model{{},"rife-4.25-fp32-sdr8","4.25","a4b9b36f8adfcbd4eac11a38c434472e55ef411e864db3aa8e21d8c2ff113bc4",64,{}};
    rifelove::gpu::DeviceInfo device{0,"NVIDIA GeForce GTX 1660 SUPER","00000000:01:00.0",7,5,126,6ULL * 1024 * 1024 * 1024};
    TensorRtCacheIdentity identity{"1.22.0","10.9.0.34","12.8.90","fp32","rife-v1-min64-opt720p-max4k",1024ULL * 1024 * 1024};
    const auto prefix = tensorRtEngineCachePrefix(model,device,identity);
    EXPECT_EQ(prefix,tensorRtEngineCachePrefix(model,device,identity));
    EXPECT_EQ(prefix.size(),69U);

    auto changedModel = model; changedModel.sha256[0] = changedModel.sha256[0] == 'a' ? 'b' : 'a';
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(changedModel,device,identity));
    auto changedIdentity = identity; changedIdentity.tensorRtVersion = "10.9.1.0";
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,device,changedIdentity));
    changedIdentity = identity; changedIdentity.cudaRuntimeVersion = "12.9.0";
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,device,changedIdentity));
    changedIdentity = identity; changedIdentity.precision = "fp16";
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,device,changedIdentity));
    changedIdentity = identity; changedIdentity.workspaceBytes /= 2;
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,device,changedIdentity));
    auto changedDevice = device; changedDevice.computeMinor = 6;
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,changedDevice,identity));
    changedDevice = device; changedDevice.driverCudaApiVersion++;
    EXPECT_NE(prefix,tensorRtEngineCachePrefix(model,changedDevice,identity));
}

TEST(TensorRtCache, NamespacesRestorationEnginesByGraphAndPipeline) {
    rifelove::gpu::DeviceInfo device{0,"NVIDIA GeForce GTX 1660 SUPER","00000000:01:00.0",7,5,126,6ULL * 1024 * 1024 * 1024};
    TensorRtCacheIdentity identity{"1.22.0","10.9.0.34","12.8.90","fp32","sr-rgb-fp32-min64-opt256-max320",512ULL * 1024 * 1024};
    const auto restoration = tensorRtEngineCachePrefix("restoration","realesrgan-x4plus-sdr8-local","1.0",
        "a4b9b36f8adfcbd4eac11a38c434472e55ef411e864db3aa8e21d8c2ff113bc4",device,identity);
    const auto rife = tensorRtEngineCachePrefix("rife","realesrgan-x4plus-sdr8-local","1.0",
        "a4b9b36f8adfcbd4eac11a38c434472e55ef411e864db3aa8e21d8c2ff113bc4",device,identity);
    EXPECT_TRUE(restoration.starts_with("restoration-"));
    EXPECT_NE(restoration,rife);
    EXPECT_EQ(restoration,tensorRtEngineCachePrefix("restoration","realesrgan-x4plus-sdr8-local","1.0",
        "a4b9b36f8adfcbd4eac11a38c434472e55ef411e864db3aa8e21d8c2ff113bc4",device,identity));
    EXPECT_NE(restoration,tensorRtEngineCachePrefix("restoration","realesrgan-x4plus-sdr8-local","1.0",
        std::string(64,'f'),device,identity));
}

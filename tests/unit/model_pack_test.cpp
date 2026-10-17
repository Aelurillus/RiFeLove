#include "inference/model_pack.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <chrono>

using namespace rifelove::inference;
TEST(ModelIntegrity, Sha256MatchesStandardVectors) {
    EXPECT_EQ(sha256({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string input = "abc";
    EXPECT_EQ(sha256(std::as_bytes(std::span(input))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST(ModelIntegrity, BoundsHashMismatchAndPathEscapesAreRejected) {
    const auto root = std::filesystem::temp_directory_path() / ("rifelove-model-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    { std::ofstream output(root / "model.onnx", std::ios::binary); output << "abc"; }
    EXPECT_EQ(readVerifiedFile(root / "model.onnx", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 3).size(),3);
    EXPECT_THROW((void)readLocalFile(root / "model.onnx", 2), std::runtime_error);
    EXPECT_THROW((void)readVerifiedFile(root / "model.onnx", std::string(64,'0'), 3), std::runtime_error);
    EXPECT_THROW((void)packFile(root, "../outside.onnx"), std::invalid_argument);
    EXPECT_THROW((void)packFile(root, "C:/outside.onnx"), std::invalid_argument);
    EXPECT_THROW((void)packFile(root, "model.onnx:stream"), std::invalid_argument);
    EXPECT_EQ(packFile(root, "model.onnx"), std::filesystem::canonical(root / "model.onnx"));
}
TEST(ModelIntegrity, UnicodePackContractAndChangedGraphAreCheckedBeforeInference) {
    auto name = std::filesystem::path(u8"rifelove-\u6a21\u578b-");
    name += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / name;
    ASSERT_TRUE(std::filesystem::create_directory(root));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
    { std::ofstream file(root / "model.onnx",std::ios::binary); file << "abc"; }
    { std::ofstream file(root / "LICENSE.txt",std::ios::binary); file << "MIT fixture"; }
    const std::string descriptor = R"({"schemaVersion":1,"id":"fixture","family":"rife","version":"4.25",
        "modelFile":"model.onnx","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "license":"MIT","licenseFile":"LICENSE.txt","inputLayout":"NCHW","dtype":"float32","batch":1,"channels":3,
        "inputRange":[0.0,1.0],"spatialMultiple":64,"timestepShape":[1,1,1,1],"dynamicTimestep":true,"dynamicSpatial":true})";
    const auto writeDescriptor = [&](const std::string& text) { std::ofstream file(root / "model.json",std::ios::binary); file << text; };
    writeDescriptor(descriptor);
    EXPECT_EQ(loadModelPack(root).graph.size(),3);
    auto wrongType = descriptor; wrongType.replace(wrongType.find("float32"),7,"float16");
    writeDescriptor(wrongType);
    EXPECT_THROW((void)loadModelPack(root),std::invalid_argument);
    writeDescriptor(descriptor);
    { std::ofstream file(root / "model.onnx",std::ios::binary); file << "modified graph"; }
    EXPECT_THROW((void)loadModelPack(root),std::runtime_error);
}

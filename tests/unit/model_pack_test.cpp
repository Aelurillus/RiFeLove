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

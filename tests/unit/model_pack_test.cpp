#include "inference/model_pack.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <span>

using namespace rifelove::inference;
using Json = nlohmann::json;

namespace {
struct TemporaryDirectory {
    std::filesystem::path value;
    explicit TemporaryDirectory(const char* prefix) {
        value = std::filesystem::temp_directory_path() /
            (std::string(prefix) + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(value)) throw std::runtime_error("Could not create model test directory");
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(value); }
};

std::string hashText(const std::string& value) {
    return sha256(std::as_bytes(std::span<const char>(value.data(), value.size())));
}
void writeText(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    if (!output) throw std::runtime_error("Could not write model test file");
}
Json readJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Json::parse(input);
}
void writeJson(const std::filesystem::path& path, const Json& value) { writeText(path, value.dump(2) + "\n"); }

void makeV2Pack(const std::filesystem::path& root) {
    const std::string graph = "abc";
    const std::string license = "MIT fixture";
    const std::string manifest = Json{
        {"schemaVersion", 1}, {"modelId", "rife-4.25"},
        {"upstreamRepository", "https://github.com/hzwer/Practical-RIFE"},
        {"upstreamCommit", "abcdef0123456789"}, {"license", "MIT"},
        {"licenseScopeEvidence", "https://example.invalid/license-scope"}
    }.dump(2) + "\n";
    const std::vector<std::string> backends{"pytorch_cpu", "onnxruntime_cpu"};
    const std::vector<std::string> profiles{"bt709-sdr8","bt470bg-sdr8","bt709-sdr10","bt470bg-sdr10"};
    const std::string validation = Json{
        {"status", "passed_cpu_reference"}, {"model_sha256", hashText(graph)},
        {"numerical_profile", "sdr8"}, {"model_adapter",{{"id","rife-sdr-v1"},{"version",1}}},
        {"validated_backends", backends}, {"supported_input_profiles",profiles},
        {"cases", Json::array({Json{{"passed", true}}})}
    }.dump(2) + "\n";
    writeText(root / "model.onnx", graph);
    writeText(root / "LICENSE.txt", license);
    writeText(root / "source-manifest.json", manifest);
    writeText(root / "validation.json", validation);
    const Json descriptor{
        {"schemaVersion", 2}, {"id", "rife-4.25-fp32-sdr8"}, {"displayName", "RIFE 4.25 FP32 SDR8"},
        {"family", "rife"}, {"version", "4.25"}, {"adapter",{{"id","rife-sdr-v1"},{"version",1}}},
        {"modelFile", "model.onnx"}, {"sha256", hashText(graph)},
        {"license", {{"id", "MIT"}, {"file", "LICENSE.txt"}, {"sha256", hashText(license)}}},
        {"provenance", {{"manifestFile", "source-manifest.json"}, {"manifestSha256", hashText(manifest)}}},
        {"capabilities", {
            {"inputLayout", "NCHW"}, {"dtype", "float32"}, {"batch", 1}, {"channels", 3},
            {"inputRange", Json::array({0.0, 1.0})}, {"spatialMultiple", 64},
            {"timestepShape", Json::array({1, 1, 1, 1})}, {"dynamicTimestep", true}, {"dynamicSpatial", true},
            {"supportedInputProfiles", profiles}, {"hdrQualified", false}
        }},
        {"validation", {
            {"reportFile", "validation.json"}, {"reportSha256", hashText(validation)},
            {"numericalProfile", "sdr8"}, {"validatedBackends", backends}
        }}
    };
    writeJson(root / "model.json", descriptor);
}
}

TEST(ModelIntegrity, Sha256MatchesStandardVectors) {
    EXPECT_EQ(sha256({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string input = "abc";
    EXPECT_EQ(sha256(std::as_bytes(std::span(input))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(ModelIntegrity, BoundsHashMismatchAndPathEscapesAreRejected) {
    TemporaryDirectory temporary("rifelove-model-test-");
    { std::ofstream output(temporary.value / "model.onnx", std::ios::binary); output << "abc"; }
    EXPECT_EQ(readVerifiedFile(temporary.value / "model.onnx", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 3).size(), 3);
    EXPECT_THROW((void)readLocalFile(temporary.value / "model.onnx", 2), std::runtime_error);
    try {
        (void)readLocalFile(temporary.value / "missing.onnx", 10);
        FAIL() << "A missing local model file was accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("missing.onnx"),std::string::npos);
    }
    EXPECT_THROW((void)readVerifiedFile(temporary.value / "model.onnx", std::string(64, '0'), 3), std::runtime_error);
    EXPECT_THROW((void)packFile(temporary.value, "../outside.onnx"), std::invalid_argument);
    EXPECT_THROW((void)packFile(temporary.value, "C:/outside.onnx"), std::invalid_argument);
    EXPECT_THROW((void)packFile(temporary.value, "model.onnx:stream"), std::invalid_argument);
    EXPECT_EQ(packFile(temporary.value, "model.onnx"), std::filesystem::canonical(temporary.value / "model.onnx"));
}

TEST(ModelIntegrity, LegacyV1PackStillLoadsWithoutClaimingSidecarVerification) {
    TemporaryDirectory temporary("rifelove-legacy-pack-");
    writeText(temporary.value / "model.onnx", "abc");
    writeText(temporary.value / "LICENSE.txt", "MIT fixture");
    const std::string descriptor = R"({"schemaVersion":1,"id":"fixture","family":"rife","version":"4.25",
        "modelFile":"model.onnx","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "license":"MIT","licenseFile":"LICENSE.txt","inputLayout":"NCHW","dtype":"float32","batch":1,"channels":3,
        "inputRange":[0.0,1.0],"spatialMultiple":64,"timestepShape":[1,1,1,1],"dynamicTimestep":true,"dynamicSpatial":true,
        "validatedBackends":["onnxruntime_cpu"],"numericalProfile":"sdr8","hdrQualified":false})";
    writeText(temporary.value / "model.json", descriptor);
    const auto model = loadModelPack(temporary.value);
    EXPECT_EQ(model.schemaVersion, 1);
    EXPECT_EQ(model.displayName, "fixture");
    EXPECT_EQ(model.licenseId, "MIT");
    EXPECT_EQ(model.validatedBackends, std::vector<std::string>{"onnxruntime_cpu"});
    EXPECT_FALSE(model.metadataIntegrityVerified);
    EXPECT_EQ(model.graph.size(), 3);
}

TEST(ModelIntegrity, V2PackVerifiesWeightsLicenseProvenanceAndValidation) {
    TemporaryDirectory temporary("rifelove-v2-pack-");
    makeV2Pack(temporary.value);
    const auto model = loadModelPack(temporary.value);
    EXPECT_EQ(model.schemaVersion, 2);
    EXPECT_EQ(model.id, "rife-4.25-fp32-sdr8");
    EXPECT_EQ(model.displayName, "RIFE 4.25 FP32 SDR8");
    EXPECT_EQ(model.version, "4.25");
    EXPECT_EQ(model.adapterId, "rife-sdr-v1");
    EXPECT_EQ(model.adapterVersion, 1);
    EXPECT_EQ(model.licenseId, "MIT");
    EXPECT_EQ(model.sourceRepository, "https://github.com/hzwer/Practical-RIFE");
    EXPECT_EQ(model.upstreamCommit, "abcdef0123456789");
    EXPECT_EQ(model.numericalProfile, "sdr8");
    EXPECT_EQ(model.validatedBackends, (std::vector<std::string>{"pytorch_cpu", "onnxruntime_cpu"}));
    EXPECT_EQ(model.supportedInputProfiles, (std::vector<std::string>{"bt709-sdr8","bt470bg-sdr8","bt709-sdr10","bt470bg-sdr10"}));
    EXPECT_FALSE(model.hdrQualified);
    EXPECT_TRUE(model.metadataIntegrityVerified);
}

TEST(ModelIntegrity, RejectsAdapterVersionThatDoesNotMatchNumericalEvidence) {
    TemporaryDirectory temporary("rifelove-v2-adapter-");
    makeV2Pack(temporary.value);
    auto descriptor = readJson(temporary.value / "model.json");
    descriptor["adapter"]["version"] = 2;
    writeJson(temporary.value / "model.json",descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value),std::invalid_argument);
}

TEST(ModelIntegrity, InputProfileMatchingKeepsLegacyPacksToSdr8) {
    ModelPack current; current.supportedInputProfiles = {"bt709-sdr8","bt709-sdr10"};
    EXPECT_TRUE(supportsInputProfile(current,"bt709-sdr8"));
    EXPECT_TRUE(supportsInputProfile(current,"bt709-sdr10"));
    EXPECT_FALSE(supportsInputProfile(current,"bt470bg-sdr10"));
    ModelPack legacy; legacy.supportedInputProfiles = {"legacy-unverified"};
    EXPECT_TRUE(supportsInputProfile(legacy,"bt709-sdr8"));
    EXPECT_FALSE(supportsInputProfile(legacy,"bt709-sdr10"));
}

TEST(ModelIntegrity, V2PackRejectsChangedSidecarsAndEscapingPaths) {
    TemporaryDirectory temporary("rifelove-v2-tamper-");
    for (const auto* name : {"LICENSE.txt", "source-manifest.json", "validation.json"}) {
        makeV2Pack(temporary.value);
        writeText(temporary.value / name, "changed after manifest creation");
        EXPECT_THROW((void)loadModelPack(temporary.value), std::runtime_error) << name;
    }
    makeV2Pack(temporary.value);
    auto descriptor = readJson(temporary.value / "model.json");
    descriptor["provenance"]["manifestFile"] = "../outside.json";
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value), std::invalid_argument);
}

TEST(ModelIntegrity, V2ValidationMustMatchTheGraphAndDeclaredBackends) {
    TemporaryDirectory temporary("rifelove-v2-report-");
    makeV2Pack(temporary.value);
    auto descriptor = readJson(temporary.value / "model.json");
    auto report = readJson(temporary.value / "validation.json");
    report["model_sha256"] = std::string(64, '0');
    writeJson(temporary.value / "validation.json", report);
    const auto reportBytes = readLocalFile(temporary.value / "validation.json", 1024 * 1024);
    descriptor["validation"]["reportSha256"] = sha256(reportBytes);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value), std::invalid_argument);

    makeV2Pack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    report = readJson(temporary.value / "validation.json");
    report["validated_backends"] = Json::array({"onnxruntime_cuda"});
    writeJson(temporary.value / "validation.json", report);
    const auto backendReportBytes = readLocalFile(temporary.value / "validation.json", 1024 * 1024);
    descriptor["validation"]["reportSha256"] = sha256(backendReportBytes);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value), std::invalid_argument);

    makeV2Pack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    report = readJson(temporary.value / "validation.json");
    report["supported_input_profiles"] = Json::array({"bt709-sdr8"});
    writeJson(temporary.value / "validation.json", report);
    const auto profileReportBytes = readLocalFile(temporary.value / "validation.json", 1024 * 1024);
    descriptor["validation"]["reportSha256"] = sha256(profileReportBytes);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value), std::invalid_argument);

    makeV2Pack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    report = readJson(temporary.value / "validation.json");
    report["cases"][0]["passed"] = false;
    writeJson(temporary.value / "validation.json", report);
    const auto failedReportBytes = readLocalFile(temporary.value / "validation.json", 1024 * 1024);
    descriptor["validation"]["reportSha256"] = sha256(failedReportBytes);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)loadModelPack(temporary.value), std::invalid_argument);
}

TEST(ModelIntegrity, ChangedGraphIsRejectedBeforeInference) {
    TemporaryDirectory temporary("rifelove-changed-graph-");
    makeV2Pack(temporary.value);
    writeText(temporary.value / "model.onnx", "modified graph");
    EXPECT_THROW((void)loadModelPack(temporary.value), std::runtime_error);
}

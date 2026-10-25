#include "restoration/model_pack.hpp"
#include "inference/model_pack.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <span>

using namespace rifelove;
using Json = nlohmann::json;

namespace {
struct TemporaryDirectory {
    std::filesystem::path value;
    TemporaryDirectory() {
        value = std::filesystem::temp_directory_path() /
            ("rifelove-restoration-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(value)) throw std::runtime_error("Could not create restoration model test directory");
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(value); }
};

std::string hashText(const std::string& value) {
    return inference::sha256(std::as_bytes(std::span<const char>(value.data(), value.size())));
}
void writeText(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    if (!output) throw std::runtime_error("Could not write restoration model test file");
}
Json readJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return Json::parse(input);
}
void writeJson(const std::filesystem::path& path, const Json& value) { writeText(path, value.dump(2) + "\n"); }

void makePack(const std::filesystem::path& root) {
    const std::string graph = "onnx graph fixture";
    const std::string license = "MIT fixture license";
    const std::string manifest = Json{
        {"schemaVersion", 1}, {"modelId", "fixture-restoration"},
        {"upstreamRepository", "https://example.invalid/restoration"},
        {"upstreamCommit", "abcdef0123456789"}, {"license", "MIT"},
        {"licenseScopeEvidence", "https://example.invalid/license-scope"}
    }.dump(2) + "\n";
    const Json capabilities{
        {"tasks", Json::array({"denoise"})},
        {"placements", Json::array({"source-rate-before-interpolation"})},
        {"supportedInputProfiles", Json::array({"bt709-sdr8"})},
        {"temporalFramesBefore", 1}, {"temporalFramesAfter", 1},
        {"scaleNumerator", 1}, {"scaleDenominator", 1},
        {"supportsBoundaryPadding", true}
    };
    const std::vector<std::string> backends{"onnxruntime_cpu"};
    const std::string validation = Json{
        {"status", "passed_cpu_reference"}, {"model_sha256", hashText(graph)},
        {"model_adapter", {{"id", "fixture-denoiser-v1"}, {"version", 1}}},
        {"numerical_profile", "sdr8-reference"}, {"validated_backends", backends},
        {"capabilities", capabilities}, {"cases", Json::array({Json{{"case_id", "paired-small"}, {"passed", true}}})}
    }.dump(2) + "\n";
    writeText(root / "weights.onnx", graph);
    writeText(root / "LICENSE.txt", license);
    writeText(root / "source-manifest.json", manifest);
    writeText(root / "validation.json", validation);
    writeJson(root / "model.json", Json{
        {"schemaVersion", 1}, {"id", "fixture-restoration"}, {"displayName", "Fixture Denoiser"},
        {"family", "video-restoration"}, {"version", "1.0"},
        {"adapter", {{"id", "fixture-denoiser-v1"}, {"version", 1}}},
        {"modelFile", "weights.onnx"}, {"sha256", hashText(graph)},
        {"license", {{"id", "MIT"}, {"file", "LICENSE.txt"}, {"sha256", hashText(license)}}},
        {"provenance", {{"manifestFile", "source-manifest.json"}, {"manifestSha256", hashText(manifest)}}},
        {"capabilities", capabilities},
        {"validation", {{"reportFile", "validation.json"}, {"reportSha256", hashText(validation)},
            {"numericalProfile", "sdr8-reference"}, {"validatedBackends", backends}}}
    });
}
}

TEST(RestorationModelPack, LoadsLocalWeightsAndHashBoundSidecars) {
    TemporaryDirectory temporary;
    makePack(temporary.value);
    const auto pack = restoration::loadModelPack(temporary.value);
    EXPECT_EQ(pack.id, "fixture-restoration");
    EXPECT_EQ(pack.displayName, "Fixture Denoiser");
    EXPECT_EQ(pack.version, "1.0");
    EXPECT_EQ(pack.graphPath, std::filesystem::canonical(temporary.value / "weights.onnx"));
    EXPECT_EQ(pack.licenseId, "MIT");
    EXPECT_EQ(pack.numericalProfile, "sdr8-reference");
    EXPECT_EQ(pack.sourceRepository, "https://example.invalid/restoration");
    EXPECT_EQ(pack.capabilities.tasks, (std::vector<restoration::Task>{restoration::Task::denoise}));
    EXPECT_EQ(pack.capabilities.temporalFramesBefore, 1U);
    EXPECT_EQ(pack.validatedBackends, (std::vector<std::string>{"onnxruntime_cpu"}));
    EXPECT_TRUE(pack.metadataIntegrityVerified);
}

TEST(RestorationModelPack, RejectsChangedGraphAndSidecars) {
    for (const auto* file : {"weights.onnx", "LICENSE.txt", "source-manifest.json", "validation.json"}) {
        TemporaryDirectory temporary;
        makePack(temporary.value);
        writeText(temporary.value / file, "modified");
        EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::runtime_error) << file;
    }
}

TEST(RestorationModelPack, ReportMustMatchAdapterCapabilitiesAndPassedCases) {
    TemporaryDirectory temporary;
    makePack(temporary.value);
    auto descriptor = readJson(temporary.value / "model.json");
    auto report = readJson(temporary.value / "validation.json");
    report["capabilities"]["temporalFramesAfter"] = 0;
    writeJson(temporary.value / "validation.json", report);
    descriptor["validation"]["reportSha256"] = inference::sha256File(temporary.value / "validation.json", 1024 * 1024);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::invalid_argument);

    makePack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    report = readJson(temporary.value / "validation.json");
    report["cases"][0]["passed"] = false;
    writeJson(temporary.value / "validation.json", report);
    descriptor["validation"]["reportSha256"] = inference::sha256File(temporary.value / "validation.json", 1024 * 1024);
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::invalid_argument);
}

TEST(RestorationModelPack, RejectsUnsafePathsUnknownProfilesAndInvalidScale) {
    TemporaryDirectory temporary;
    makePack(temporary.value);
    auto descriptor = readJson(temporary.value / "model.json");
    descriptor["modelFile"] = "../weights.onnx";
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::invalid_argument);

    makePack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    descriptor["capabilities"]["supportedInputProfiles"] = Json::array({"pq-hdr10"});
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::invalid_argument);

    makePack(temporary.value);
    descriptor = readJson(temporary.value / "model.json");
    descriptor["capabilities"]["scaleNumerator"] = 2;
    writeJson(temporary.value / "model.json", descriptor);
    EXPECT_THROW((void)restoration::loadModelPack(temporary.value), std::invalid_argument);
}

TEST(ModelIntegrity, FileSha256StreamsAndEnforcesTheDeclaredLimit) {
    TemporaryDirectory temporary;
    writeText(temporary.value / "small.onnx", "abc");
    EXPECT_EQ(inference::sha256File(temporary.value / "small.onnx", 3), hashText("abc"));
    EXPECT_THROW((void)inference::sha256File(temporary.value / "small.onnx", 2), std::runtime_error);
}

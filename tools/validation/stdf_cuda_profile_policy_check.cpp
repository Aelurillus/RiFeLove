#include "restoration/stdf_cuda_backend.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;
using rifelove::restoration::StdfCudaDiagnostics;

Json kernel(std::string op, std::optional<std::string> provider,
            std::optional<Json> outputShape = std::nullopt) {
    Json args = Json::object();
    if (provider) args["provider"] = *provider;
    args["op_name"] = std::move(op);
    if (outputShape) args["output_type_shape"] = std::move(*outputShape);
    return Json{{"cat", "Node"}, {"name", "/test_kernel_time"}, {"args", std::move(args)}};
}

Json imageShape() { return Json::array({Json{{"float", Json::array({1, 1, 64, 64})}}}); }
Json scalarShape() { return Json::array({Json{{"int64", Json::array({4})}}}); }

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

StdfCudaDiagnostics inspect(const std::filesystem::path& path, Json events) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "could not create synthetic profile");
    output << events.dump() << '\n';
    output.close();
    return rifelove::restoration::inspectStdfCudaProfile(path);
}

void expectValid(const std::filesystem::path& path) {
    Json events = Json::array();
    for (std::size_t index = 0; index < rifelove::validation::stdf::qualifiedCudaConvKernelCount; ++index)
        events.push_back(kernel("Conv", "CUDAExecutionProvider", imageShape()));
    for (std::size_t index = 0; index < rifelove::validation::stdf::qualifiedCudaGridSampleKernelCount; ++index)
        events.push_back(kernel("GridSample", "CUDAExecutionProvider", imageShape()));
    events.push_back(kernel("Gather", "CPUExecutionProvider", scalarShape()));
    events.push_back(Json{{"cat", "Node"}, {"name", "/metadata"}, {"args", Json::object()}});
    const auto diagnostics = inspect(path, std::move(events));
    require(diagnostics.criticalComputeVerified, "valid CUDA Conv/GridSample profile was rejected");
    require(diagnostics.cudaConvNodeCount == rifelove::validation::stdf::qualifiedCudaConvKernelCount &&
                diagnostics.cudaGridSampleNodeCount == rifelove::validation::stdf::qualifiedCudaGridSampleKernelCount,
            "valid profile did not count CUDA image kernels");
    require(diagnostics.cpuControlNodeCount == 1 && diagnostics.unknownProviderKernelCount == 0,
            "valid profile misclassified CPU control or non-kernel events");
}

void expectRejected(const std::filesystem::path& path, Json events, const std::string& caseName) {
    const auto diagnostics = inspect(path, std::move(events));
    require(!diagnostics.criticalComputeVerified, caseName + " was accepted");
}

void expectStickyProfileFailure() {
    rifelove::restoration::StdfCudaProfileGateState state;
    StdfCudaDiagnostics rejected;
    rejected.fallbackReason = "synthetic profile rejection";
    std::string reason;
    require(!rifelove::restoration::acceptStdfCudaProfile(state, rejected, true, reason),
            "first failed profile was accepted");
    require(state.failed && !state.verified && !reason.empty(),
            "first failed profile did not enter the terminal failure state");

    StdfCudaDiagnostics valid;
    valid.criticalComputeVerified = true;
    reason.clear();
    require(!rifelove::restoration::acceptStdfCudaProfile(state, valid, true, reason),
            "a later valid profile bypassed the terminal failure state");
    require(state.failed && !state.verified,
            "profile gate became verified after a prior failure");
}

void expectOutputContractBoundaries() {
    const auto valid = rifelove::restoration::StdfCudaOutputTensorMetadata{
        true, true, 4, {1, 1, 64, 64}, 64 * 64};
    rifelove::restoration::validateStdfCudaOutputTensorContract(valid, 64, 64, 64 * 64);

    const auto expectRejectedOutput = [](auto metadata, const std::string& caseName) {
        bool rejected = false;
        try {
            rifelove::restoration::validateStdfCudaOutputTensorContract(metadata, 64, 64, 64 * 64);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, caseName + " was accepted");
    };
    auto nonTensor = valid; nonTensor.isTensor = false;
    expectRejectedOutput(nonTensor, "non-tensor output");
    auto nonFp32 = valid; nonFp32.isFp32 = false;
    expectRejectedOutput(nonFp32, "non-FP32 output");
    auto wrongRank = valid; wrongRank.rank = 3;
    expectRejectedOutput(wrongRank, "wrong-rank output");
    auto wrongShape = valid; wrongShape.shape[2] = 32;
    expectRejectedOutput(wrongShape, "wrong-shape output");
    auto wrongCount = valid; wrongCount.elementCount -= 1;
    expectRejectedOutput(wrongCount, "wrong-element-count output");
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "RifeLove" / "stdf-profile-policy-check";
        std::error_code error;
        std::filesystem::create_directories(root, error);
        require(!error, "could not create profile test directory");
        const auto path = root / "profile.json";

        expectValid(path);
        expectRejected(path, Json::array({
            kernel("Conv", std::nullopt, imageShape()),
            kernel("GridSample", "CUDAExecutionProvider", imageShape()),
        }), "missing provider profile");
        expectRejected(path, Json::array({
            kernel("Conv", "CUDAExecutionProvider", imageShape()),
            kernel("GridSample", "CUDAExecutionProvider", imageShape()),
            kernel("Gather", "CPUExecutionProvider"),
        }), "missing CPU output shape profile");
        expectRejected(path, Json::array({
            kernel("Conv", "CPUExecutionProvider", imageShape()),
            kernel("GridSample", "CUDAExecutionProvider", imageShape()),
        }), "CPU Conv profile");
        expectRejected(path, Json::array({kernel("Conv", "CUDAExecutionProvider", imageShape())}),
                       "missing GridSample profile");
        expectRejected(path, Json::array({
            kernel("Conv", "VulkanExecutionProvider", imageShape()),
            kernel("GridSample", "CUDAExecutionProvider", imageShape()),
        }), "unknown provider profile");
        Json extraConv = Json::array();
        for (std::size_t index = 0; index < rifelove::validation::stdf::qualifiedCudaConvKernelCount + 1; ++index)
            extraConv.push_back(kernel("Conv", "CUDAExecutionProvider", imageShape()));
        for (std::size_t index = 0; index < rifelove::validation::stdf::qualifiedCudaGridSampleKernelCount; ++index)
            extraConv.push_back(kernel("GridSample", "CUDAExecutionProvider", imageShape()));
        expectRejected(path, std::move(extraConv), "unexpected extra CUDA kernel profile");
        expectStickyProfileFailure();
        expectOutputContractBoundaries();

        std::filesystem::remove(path, error);
        std::filesystem::remove(root, error);
        std::cout << "STDF CUDA profile policy boundary checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

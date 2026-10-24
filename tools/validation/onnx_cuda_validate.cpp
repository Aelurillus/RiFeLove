#include "inference/onnx_cuda_backend.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>

using namespace rifelove;
using nlohmann::json;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string pathText(const std::filesystem::path& path) { const auto text = path.u8string(); return {text.begin(), text.end()}; }
json readJson(const std::filesystem::path& path) {
    const auto bytes = inference::readLocalFile(path, 16 * 1024 * 1024);
    require(!bytes.empty(), "Empty JSON validation file");
    return json::parse(reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data() + bytes.size()));
}
std::vector<std::byte> fixture(const std::filesystem::path& root, const json& item) {
    return inference::readVerifiedFile(inference::packFile(root, item.at("file").get<std::string>()), item.at("sha256").get<std::string>(), 256 * 1024 * 1024);
}
inference::GpuFrame upload(gpu::CudaDevice& device, const std::vector<std::byte>& bytes, int width, int height) {
    auto buffer = device.allocate(bytes.size());
    buffer->upload(bytes);
    return {std::move(buffer), width, height};
}
void expectInferenceError(std::future<inference::InterpolationResult> future, inference::InferenceErrorKind kind) {
    try { future.get(); } catch (const inference::InferenceError& error) { require(error.kind() == kind, "Unexpected inference error category"); return; }
    throw std::runtime_error("Invalid request unexpectedly succeeded");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 4 || argc > 5) { std::cerr << "Usage: rifelove-validate-cuda MODEL_PACK RUNTIME_DLL_DIRECTORY REPORT_JSON [cuda|tensorrt]\n"; return 2; }
    try {
        static_assert(std::endian::native == std::endian::little);
        const std::wstring requestedProvider = argc == 5 ? argv[4] : L"cuda";
        if (requestedProvider != L"cuda" && requestedProvider != L"tensorrt") throw std::invalid_argument("Provider must be cuda or tensorrt");
        const auto root = std::filesystem::absolute(std::filesystem::path(argv[1]));
        const auto reportPath = std::filesystem::absolute(std::filesystem::path(argv[3]));
        const auto pack = inference::loadModelPack(root);
        const auto reference = readJson(inference::packFile(root, "validation.json"));
        require(reference.at("status") == "passed_cpu_reference", "Reference pack has not passed CPU qualification");
        require(reference.at("model_sha256") == pack.sha256, "Reference report belongs to a different ONNX graph");
        auto device = std::make_shared<gpu::CudaDevice>(0, 128 * 1024 * 1024);
        inference::OnnxCudaConfig config;
        config.runtimeDirectory = std::filesystem::absolute(std::filesystem::path(argv[2]));
        config.providerPreference = requestedProvider == L"tensorrt" ? inference::ProviderPreference::requireTensorRt : inference::ProviderPreference::cudaOnly;
        config.profilePrefix = reportPath.parent_path() / "onnx-cuda-profile";
        inference::OnnxCudaBackend backend(device, config);
        backend.loadModel(pack);
        const auto capabilities = backend.capabilities();
        json report = {{"backend",capabilities.backend}, {"requested_provider",requestedProvider == L"tensorrt" ? "tensorrt" : "cuda"},
            {"runtime", "1.22.0"}, {"model_sha256",pack.sha256},
            {"device",device->info().name}, {"compute_capability",std::to_string(device->info().computeMajor) + "." + std::to_string(device->info().computeMinor)},
            {"use_tf32",false}, {"convolution_algorithm_search","DEFAULT"}, {"arena_budget_bytes",config.arenaBudgetBytes}, {"cases",json::array()}};
        const double maxLimit = reference.at("ort_max_abs_error_limit").get<double>();
        const double psnrLimit = reference.value("minimum_psnr_db", 80.0);
        require(maxLimit > 0 && maxLimit <= 0.5 / 255 && psnrLimit >= 80, "Reference tolerance is weaker than the SDR8 numerical gate");
        bool passed = true;
        std::size_t contractChecks = 0;
        require(reference.at("cases").size() >= 6, "Expected multiple spatial profiles and timesteps");
        for (const auto& item : reference.at("cases")) {
            const int width = item.at("shape").at(3).get<int>(), height = item.at("shape").at(2).get<int>();
            require(width > 0 && height > 0 && width <= 8192 && height <= 8192 && width % 64 == 0 && height % 64 == 0, "Invalid reference spatial profile");
            const auto& fixtures = item.at("fixtures");
            const auto first = fixture(root, fixtures.at("frame0")), second = fixture(root, fixtures.at("frame1")), expected = fixture(root, fixtures.at("expected"));
            const auto timeBytes = fixture(root, fixtures.at("timestep"));
            float timestep = 0;
            require(timeBytes.size() == sizeof(float), "Invalid reference timestep tensor size");
            std::memcpy(&timestep, timeBytes.data(), sizeof(float));
            require(timestep == item.at("timestep").get<float>(), "Reference timestep tensor disagrees with metadata");
            const auto expectedBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 * sizeof(float);
            require(width > 0 && height > 0 && expected.size() == expectedBytes && first.size() == expectedBytes && second.size() == expectedBytes, "Reference tensor size mismatch");
            auto frame0 = upload(*device, first, width, height), frame1 = upload(*device, second, width, height);
            auto reusable = device->allocate(expectedBytes);
            const auto address = reusable->address();
            auto result = backend.interpolate({frame0,frame1,timestep,std::move(reusable),{}}).get();
            require(result.reusedOutput && result.frame.pixels->address() == address, "Output buffer was not reused");
            std::vector<std::byte> actual(expectedBytes);
            result.frame.pixels->download(actual);
            double maximum = 0, total = 0, square = 0;
            for (std::size_t i = 0; i < expectedBytes; i += sizeof(float)) {
                float a = 0, b = 0;
                std::memcpy(&a, actual.data() + i, sizeof(float)); std::memcpy(&b, expected.data() + i, sizeof(float));
                require(std::isfinite(a) && std::isfinite(b) && a >= -1e-6F && a <= 1.000001F, "RIFE output has invalid normalized values");
                const auto difference = std::abs(static_cast<double>(a) - b);
                maximum = std::max(maximum,difference); total += difference; square += difference * difference;
            }
            const auto count = static_cast<double>(expectedBytes / sizeof(float));
            const double psnr = square == 0 ? 999 : -10 * std::log10(square / count);
            const bool casePassed = maximum <= maxLimit && psnr >= psnrLimit;
            passed = passed && casePassed;
            report["cases"].push_back({{"id",item.at("id")}, {"max_abs_error",maximum}, {"mean_abs_error",total / count},
                {"psnr_db",psnr}, {"elapsed_seconds",result.elapsedSeconds}, {"passed",casePassed}});
            std::cout << report["cases"].back().dump() << '\n';
            // Endpoint semantics must be exact device copies, not model guesses.
            for (float time : {0.0F, 1.0F}) {
                auto endpoint = backend.interpolate({frame0,frame1,time,{},{}}).get();
                endpoint.frame.pixels->download(actual);
                require(actual == (time == 0 ? first : second), "Endpoint differs from its input pixels");
                ++contractChecks;
            }
            std::stop_source cancellation; cancellation.request_stop();
            expectInferenceError(backend.interpolate({frame0,frame1,0.5F,{},cancellation.get_token()}), inference::InferenceErrorKind::cancelled); ++contractChecks;
            auto badShape = frame1; badShape.width += 64;
            expectInferenceError(backend.interpolate({frame0,badShape,0.5F,{},{}}), inference::InferenceErrorKind::invalidRequest); ++contractChecks;
            expectInferenceError(backend.interpolate({frame0,frame1,std::numeric_limits<float>::quiet_NaN(),{},{}}), inference::InferenceErrorKind::invalidRequest); ++contractChecks;
            auto sharedOutput = device->allocate(expectedBytes);
            expectInferenceError(backend.interpolate({frame0,frame1,0.5F,sharedOutput,{}}), inference::InferenceErrorKind::invalidRequest); ++contractChecks;
        }
        const auto profilePath = backend.finishProfiling();
        const auto profile = readJson(profilePath);
        json assignments = json::object();
        std::set<std::string> gpuHeavy;
        std::set<std::string> tensorRtHeavy;
        std::size_t cpuLargestTensor = 0;
        const std::set<std::string> heavy = {"Conv", "FusedConv", "ConvTranspose", "GridSample", "Gemm", "MatMul"};
        const auto isTensorRt = [](const std::string& provider) {
            std::string lower = provider;
            std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return lower == "tensorrtexecutionprovider";
        };
        for (const auto& event : profile) {
            if (!event.contains("args") || !event["args"].contains("provider") || !event["args"].contains("op_name")) continue;
            const auto provider = event["args"]["provider"].get<std::string>(), operation = event["args"]["op_name"].get<std::string>();
            if (!assignments.contains(provider)) assignments[provider] = json::object();
            assignments[provider][operation] = assignments[provider].value(operation,0) + 1;
            if (provider == "CPUExecutionProvider") {
                // Permit bounded shape/scalar work, but reject silently moving
                // full image/features to a CPU kernel under a CUDA label.
                for (const auto* key : {"input_type_shape", "output_type_shape"}) {
                    require(event["args"].contains(key), "CPU profile event lacks tensor-shape evidence");
                    for (const auto& tensor : event["args"][key]) for (const auto& dimensions : tensor.items()) {
                        std::size_t elements = 1;
                        for (const auto& dimension : dimensions.value()) {
                            const auto size = dimension.get<std::int64_t>();
                            require(size >= 0 && size <= 64 && (size == 0 || elements <= 64 / static_cast<std::size_t>(size)), "Image-sized CPU fallback detected");
                            elements *= static_cast<std::size_t>(size);
                        }
                        cpuLargestTensor = std::max(cpuLargestTensor,elements);
                    }
                }
            }
            if (isTensorRt(provider) && operation.starts_with("TRTKernel_")) tensorRtHeavy.insert(operation);
            if (heavy.contains(operation)) {
                require(provider == "CUDAExecutionProvider" || isTensorRt(provider), "Image compute operator fell back to CPU");
                gpuHeavy.insert(operation);
                if (isTensorRt(provider)) tensorRtHeavy.insert(operation);
            }
        }
        if (requestedProvider == L"tensorrt")
            require(!tensorRtHeavy.empty(), "TensorRT session ran without assigning any profiled image-compute operators to TensorRT");
        else
            require((gpuHeavy.contains("Conv") || gpuHeavy.contains("FusedConv")) && gpuHeavy.contains("ConvTranspose") && gpuHeavy.contains("GridSample"),
                "No profiling proof of CUDA convolution and warp execution");
        report["operator_assignments"] = assignments;
        report["tensorrt_heavy_operators"] = json::array();
        for (const auto& operation : tensorRtHeavy) report["tensorrt_heavy_operators"].push_back(operation);
        report["cpu_tensor_max_elements"] = cpuLargestTensor;
        report["profile_file"] = pathText(profilePath);
        report["contract_checks"] = contractChecks;
        report["max_abs_error_limit"] = maxLimit;
        report["minimum_psnr_db"] = psnrLimit;
        const std::string backendName = requestedProvider == L"tensorrt" ? "tensorrt" : "cuda";
        report["status"] = passed ? "passed_native_" + backendName + "_reference" : "failed_numerical_gate";
        std::ofstream output(reportPath, std::ios::binary);
        output << report.dump(2) << '\n';
        if (!output) throw std::runtime_error("Could not write validation report");
        std::cout << json({{"event","native_cuda_validation"}, {"status",report["status"]}, {"report",pathText(reportPath)}}).dump() << '\n';
        return passed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

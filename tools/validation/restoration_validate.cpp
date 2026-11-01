#include "restoration/onnx_cuda_backend.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <process.h>
#include <vector>

using namespace rifelove;

int wmain(int argc, wchar_t** argv) {
    if (argc < 7 || argc > 9) {
        std::cerr << "Usage: rifelove-validate-restoration PACK RUNTIME INPUT_RGB WIDTH HEIGHT OUTPUT_RGB [cuda|tensorrt [DEBLUR_BLEND_0_TO_1]]\n";
        return 2;
    }
    try {
        const auto width = std::stoi(argv[4]);
        const auto height = std::stoi(argv[5]);
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
            throw std::invalid_argument("Invalid RGB fixture dimensions");
        const std::wstring provider = argc >= 8 ? argv[7] : L"cuda";
        if (provider != L"cuda" && provider != L"tensorrt")
            throw std::invalid_argument("Provider must be cuda or tensorrt");
        const auto outputPath = std::filesystem::absolute(std::filesystem::path(argv[6]));
        if (std::filesystem::exists(outputPath)) throw std::runtime_error("Refusing to replace the output fixture");
        std::filesystem::create_directories(outputPath.parent_path());
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
        std::ifstream input(std::filesystem::path(argv[3]),std::ios::binary);
        input.read(reinterpret_cast<char*>(pixels->data()),static_cast<std::streamsize>(pixels->size()));
        if (!input || input.peek() != std::ifstream::traits_type::eof())
            throw std::runtime_error("RGB input size does not match its dimensions");

        media::DecodedFrame source{};
        source.width = width; source.height = height; source.index = 0;
        source.timestamp = Rational(0); source.duration = Rational(1,30);
        source.color.pixelFormat = "rgb24"; source.color.bitDepth = 8;
        source.color.primaries = "bt709"; source.color.transfer = "iec61966-2-1";
        source.color.matrix = "gbr"; source.color.range = "pc";
        source.planes[0] = {pixels->data(),width * 3}; source.storage = pixels;

        const auto pack = restoration::loadModelPack(std::filesystem::path(argv[1]));
        const auto task = restoration::outputRateImageTask(pack.capabilities);
        if (!task) throw std::runtime_error("Model does not support output-rate image restoration");
        auto device = std::make_shared<gpu::CudaDevice>(0,128ULL * 1024 * 1024);
        restoration::OnnxCudaRestorationConfig config;
        config.runtimeDirectory = std::filesystem::path(argv[2]);
        config.providerPreference = provider == L"cuda" ? inference::ProviderPreference::cudaOnly : inference::ProviderPreference::requireTensorRt;
        config.profilePrefix = outputPath.parent_path() /
            (outputPath.filename().wstring() + L"-profile-" + std::to_wstring(::_getpid()));
        if (provider == L"tensorrt")
            config.engineCacheDirectory = outputPath.parent_path() / L"engine-cache";
        restoration::OnnxCudaVideoRestorationBackend backend(device,config);
        backend.loadModel(pack);
        restoration::Request request;
        request.task = *task;
        request.placement = restoration::Placement::outputRateAfterInterpolation;
        request.context = {source}; request.centerIndex = 0; request.inputProfile = "bt709-sdr8";
        if (argc == 9) {
            std::size_t parsed = 0;
            const std::wstring value(argv[8]);
            request.deblurStrength = std::stof(value,&parsed);
            if (parsed != value.size()) throw std::invalid_argument("Deblur blend must be a numeric value in [0,1]");
        }
        restoration::validateRequest(pack.capabilities,request);
        const auto deblurStrength = request.deblurStrength;
        const auto result = backend.restore(std::move(request)).get();
        std::ofstream output(outputPath,std::ios::binary);
        for (int row = 0; row < result.centerFrame.height; ++row)
            output.write(reinterpret_cast<const char*>(result.centerFrame.planes[0].data +
                static_cast<std::ptrdiff_t>(row) * result.centerFrame.planes[0].stride),
                static_cast<std::streamsize>(result.centerFrame.width * 3));
        if (!output) throw std::runtime_error("Could not write the output RGB fixture");
        const auto profilePath = backend.finishProfiling();
        std::ifstream profileInput(profilePath,std::ios::binary);
        if (!profileInput) throw std::runtime_error("Could not open ONNX Runtime restoration profile");
        const auto profile = nlohmann::json::parse(profileInput);
        if (!profile.is_array()) throw std::runtime_error("Invalid ONNX Runtime restoration profile");
        nlohmann::json assignments = nlohmann::json::object();
        for (const auto& event : profile) {
            if (!event.contains("args") || !event.at("args").is_object() || !event.at("args").contains("provider")) continue;
            const auto providerName = event.at("args").at("provider").get<std::string>();
            assignments[providerName] = assignments.value(providerName,0) + 1;
        }
        const auto profileUtf8 = profilePath.u8string();
        std::cout << nlohmann::json{{"backend",backend.capabilities().runtimeBackend},
            {"deblur_blend",deblurStrength ? nlohmann::json(*deblurStrength) : nlohmann::json(nullptr)},
            {"device",device->info().name},{"width",result.centerFrame.width},{"height",result.centerFrame.height},
            {"elapsed_seconds",result.elapsedSeconds},{"provider_events",assignments},
            {"profile",std::string(profileUtf8.begin(),profileUtf8.end())}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

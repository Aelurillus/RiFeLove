#include "restoration/onnx_cuda_backend.hpp"
#include "restoration/temporal_window.hpp"
#include "restoration/noise_estimate.hpp"
#include "interpolation/scene_analysis.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <process.h>
#include <thread>

using namespace rifelove;

// Development-only numerical/performance harness. This is not a user-facing
// batch CLI or a replacement for the application's media and checkpoint path.
int wmain(int argc, wchar_t** argv) {
    if (argc < 9 || argc > 10) {
        std::cerr << "Usage: validate-temporal-restoration PACK RUNTIME INPUT_RGB WIDTH HEIGHT FRAMES SIGMA255|auto OUTPUT_RGB [cuda|tensorrt]\n";
        return 2;
    }
    try {
        const int width = std::stoi(argv[4]), height = std::stoi(argv[5]), frames = std::stoi(argv[6]);
        const bool automatic = std::wstring(argv[7]) == L"auto";
        const float sigma = automatic ? 0.0F : std::stof(argv[7]);
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || frames <= 0 || frames > 10000)
            throw std::invalid_argument("Invalid temporal RGB fixture dimensions/count");
        const std::wstring provider = argc == 10 ? argv[9] : L"cuda";
        if (provider != L"cuda" && provider != L"tensorrt") throw std::invalid_argument("Unknown provider");
        const auto outputPath = std::filesystem::absolute(std::filesystem::path(argv[8]));
        if (std::filesystem::exists(outputPath)) throw std::runtime_error("Refusing to replace a fixture output");
        std::filesystem::create_directories(outputPath.parent_path());
        std::ifstream input(std::filesystem::path(argv[3]),std::ios::binary);
        if (!input) throw std::runtime_error("Could not open temporal RGB fixture");
        const auto pack = restoration::loadModelPack(std::filesystem::path(argv[1]));
        if (!restoration::supportsOnnxCudaTemporalModel(pack)) throw std::runtime_error("Unsupported temporal model");
        auto device = std::make_shared<gpu::CudaDevice>(0,256ULL * 1024 * 1024);
        const auto initialMemory = device->memory();
        std::atomic<std::size_t> minimumFree{initialMemory.freeBytes};
        std::jthread sampler([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                try {
                    const auto free = device->memory().freeBytes;
                    auto current = minimumFree.load();
                    while (free < current && !minimumFree.compare_exchange_weak(current,free)) {}
                } catch (...) { /* Main thread reports actual runtime errors. */ }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        restoration::OnnxCudaRestorationConfig config;
        config.runtimeDirectory = std::filesystem::path(argv[2]);
        config.engineCacheDirectory = outputPath.parent_path() / L"engine-cache";
        config.profilePrefix = outputPath.parent_path() /
            (outputPath.filename().wstring() + L"-profile-" + std::to_wstring(::_getpid()));
        config.providerPreference = provider == L"cuda" ? inference::ProviderPreference::cudaOnly : inference::ProviderPreference::requireTensorRt;
        restoration::OnnxCudaVideoRestorationBackend backend(device,config);
        const auto started = std::chrono::steady_clock::now();
        backend.loadModel(pack);
        const double initializationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        restoration::TemporalWindow window(backend.capabilities());
        restoration::AutomaticDenoiseController automaticController({});
        struct NoiseObservation { restoration::GaussianNoiseEstimate estimate; bool cut; };
        std::vector<NoiseObservation> observations;
        std::optional<media::RifeSdrFrame> previousPrepared;
        std::ofstream output(outputPath,std::ios::binary);
        if (!output) throw std::runtime_error("Could not create temporal output fixture");
        nlohmann::json timings = nlohmann::json::array();
        std::size_t maximumWindow = 0;
        auto process = [&](std::vector<restoration::Request> requests) {
            for (auto& request : requests) {
                const auto expectedIndex = request.context[request.centerIndex].index;
                const auto expectedTimestamp = request.context[request.centerIndex].timestamp;
                const auto& observation = observations.at(expectedIndex);
                request.denoiseSigma255 = automatic ? automaticController.update(*expectedTimestamp,observation.estimate,observation.cut).sigma255 : sigma;
                const auto actualSigma = *request.denoiseSigma255;
                auto restored = backend.restore(std::move(request)).get();
                if (restored.centerFrame.index != expectedIndex || restored.centerFrame.timestamp != expectedTimestamp)
                    throw std::runtime_error("Temporal restoration shifted frame identity or PTS");
                for (int y = 0; y < height; ++y)
                    output.write(reinterpret_cast<const char*>(restored.centerFrame.planes[0].data +
                        static_cast<std::ptrdiff_t>(y) * restored.centerFrame.planes[0].stride),width * 3);
                timings.push_back({{"frame",expectedIndex},{"seconds",restored.elapsedSeconds},{"sigma255",actualSigma},
                    {"estimated_sigma255",observation.estimate.sigma255},{"confidence",observation.estimate.confidence},{"scene_cut",observation.cut}});
            }
        };
        for (int index = 0; index < frames; ++index) {
            auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
            input.read(reinterpret_cast<char*>(pixels->data()),static_cast<std::streamsize>(pixels->size()));
            if (!input) throw std::runtime_error("Truncated RGB input fixture");
            media::DecodedFrame frame{};
            frame.index = static_cast<std::size_t>(index); frame.width = width; frame.height = height;
            frame.timestamp = Rational(index,30); frame.duration = Rational(1,30);
            frame.color.pixelFormat = "rgb24"; frame.color.bitDepth = 8; frame.color.primaries = "bt709";
            frame.color.transfer = "iec61966-2-1"; frame.color.matrix = "gbr"; frame.color.range = "pc";
            frame.planes[0] = {pixels->data(),width * 3}; frame.storage = pixels;
            bool cut = false;
            auto prepared = media::prepareRifeSdr(frame);
            const auto view = [](const media::RifeSdrFrame& value) {
                return quality::RgbView{value.pixels,value.width,value.height,value.paddedWidth,
                    static_cast<std::size_t>(value.paddedWidth) * static_cast<std::size_t>(value.paddedHeight)};
            };
            if (previousPrepared) cut = quality::analyzePair(view(*previousPrepared),view(prepared)).action == quality::PairAction::sceneCutHold;
            observations.push_back({restoration::estimateGaussianNoise(prepared),cut});
            previousPrepared = std::move(prepared);
            process(window.push(std::move(frame),cut));
            maximumWindow = std::max(maximumWindow,window.bufferedFrames());
        }
        if (input.peek() != std::ifstream::traits_type::eof()) throw std::runtime_error("RGB fixture contains extra input");
        process(window.finish());
        if (!output) throw std::runtime_error("Could not write temporal output fixture");
        const double totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto profilePath = backend.finishProfiling();
        std::ifstream profileInput(profilePath,std::ios::binary);
        if (!profileInput) throw std::runtime_error("Could not open ONNX Runtime temporal restoration profile");
        const auto profile = nlohmann::json::parse(profileInput);
        if (!profile.is_array()) throw std::runtime_error("Invalid ONNX Runtime temporal restoration profile");
        nlohmann::json assignments = nlohmann::json::object();
        for (const auto& event : profile)
            if (event.contains("args") && event.at("args").contains("provider")) {
                const auto name = event.at("args").at("provider").get<std::string>();
                assignments[name] = assignments.value(name,0) + 1;
            }
        sampler.request_stop(); sampler.join();
        std::cout << nlohmann::json{{"backend",backend.capabilities().runtimeBackend},
            {"runtime_signature",backend.capabilities().runtimeSignature},{"model_sha256",pack.graphSha256},
            {"device",device->info().name},{"width",width},{"height",height},{"frames",frames},
            {"noise_sigma_255",sigma},{"initialization_seconds",initializationSeconds},{"total_seconds",totalSeconds},
            {"automatic_noise_conditioning",automatic},{"automatic_algorithm_version",restoration::automaticDenoiseAlgorithmVersion},
            {"maximum_buffered_frames",maximumWindow},{"frames_per_second",frames / totalSeconds},
            {"baseline_device_used_bytes",initialMemory.totalBytes - initialMemory.freeBytes},
            {"sampled_device_peak_used_bytes",initialMemory.totalBytes - minimumFree.load()},
            {"device_memory_sampling_ms",20},{"memory_limitation","Device-wide sampled peak includes other processes; not an allocator-exact or exclusive VRAM measurement."},
            {"provider_events",assignments},{"frame_timings",timings}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// Standalone native CUDA check for the experimental STDF backend.
#include "restoration/stdf_cuda_backend.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace r = rifelove::restoration;
namespace a = rifelove::validation::stdf;
using rifelove::Rational;
using Json = nlohmann::ordered_json;

struct Storage {
    std::array<std::vector<std::uint8_t>, 3> planes;
};

struct Fixture {
    r::Request request;
    std::vector<a::BoundedPlanes> extents;
    a::SceneBounds scene;
};

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

Fixture makeFixture() {
    Fixture fixture; fixture.scene = {0, 6, 42};
    auto& request = fixture.request;
    request.task = r::Task::deblock; request.placement = r::Placement::sourceRateBeforeInterpolation;
    request.inputProfile = "bt709-sdr8"; request.centerIndex = 3;
    for (std::size_t index = 0; index < 7; ++index) {
        rifelove::media::DecodedFrame frame{}; frame.index = index; frame.width = 64; frame.height = 64;
        frame.timestamp = Rational(static_cast<std::int64_t>(index * 1001), 30000);
        frame.duration = Rational(1001, 30000); frame.color = {"yuv420p",8,"bt709","bt709","bt709","tv",false,{}, {}};
        auto storage = std::make_shared<Storage>(); a::BoundedPlanes bounds;
        for (int plane = 0; plane < 3; ++plane) {
            const int width = plane == 0 ? 64 : 32; const int height = plane == 0 ? 64 : 32; const int stride = width + 7 + plane;
            storage->planes[plane].resize(static_cast<std::size_t>(stride) * height, 0xA5);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                storage->planes[plane][static_cast<std::size_t>(y) * stride + x] = static_cast<std::uint8_t>(
                    plane == 0 ? (16 + (x * 5 + y * 3 + static_cast<int>(index) * 7) % 220)
                               : (80 + (x + y + static_cast<int>(index) * 9 + plane * 23) % 96));
            frame.planes[plane] = {storage->planes[plane].data(), stride,
                                   rifelove::media::FramePlaneExtent{storage->planes[plane].data(), storage->planes[plane].size(), 0}};
            bounds[plane] = storage->planes[plane];
        }
        frame.storage = storage; request.context.push_back(std::move(frame)); fixture.extents.push_back(bounds);
    }
    return fixture;
}

std::vector<std::uint8_t> visible(const rifelove::media::DecodedFrame& frame, int plane) {
    const int width = plane == 0 ? frame.width : frame.width / 2; const int height = plane == 0 ? frame.height : frame.height / 2;
    std::vector<std::uint8_t> result; result.reserve(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = frame.planes[plane].data + static_cast<std::size_t>(y) * frame.planes[plane].stride;
        result.insert(result.end(), row, row + width);
    }
    return result;
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: stdf_cuda_backend_check graph.onnx runtime-directory report.json");
        const fs::path graph = argv[1], runtime = argv[2], reportPath = argv[3];
        Fixture fixture = makeFixture(); const auto centerU = visible(fixture.request.context[3], 1);
        const auto centerV = visible(fixture.request.context[3], 2); const auto centerPts = fixture.request.context[3].timestamp;
        r::StdfCudaConfig config; config.runtimeDirectory = runtime; config.profilePrefix = reportPath.parent_path() / "stdf-cuda-ort-profile";
        r::StdfCudaBackend backend(config); backend.loadModel(graph);
        const auto result = backend.restore(fixture.request, fixture.extents, fixture.scene, 1);
        require(backend.diagnostics().cudaProviderAvailable && backend.diagnostics().cudaSessionCreated,
                "CUDA provider/session was not created");
        require(backend.diagnostics().criticalComputeVerified && !backend.diagnostics().cpuImageFallbackDetected,
                "profile did not prove CUDA image execution");
        require(result.index == fixture.request.context[3].index && result.timestamp == centerPts,
                "center index/PTS changed");
        require(visible(result, 1) == centerU && visible(result, 2) == centerV, "center U/V changed");
        std::stop_source stop; auto cancelled = fixture; cancelled.request.stop = stop.get_token(); stop.request_stop();
        bool rejected = false;
        try { (void)backend.restore(cancelled.request, cancelled.extents, cancelled.scene, 1); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "pre-cancelled request was accepted");
        const auto report = Json{{"passed", true}, {"runtimeVersion", backend.diagnostics().runtimeVersion},
            {"backend", backend.diagnostics().backend}, {"cudaProviderAvailable", backend.diagnostics().cudaProviderAvailable},
            {"cudaSessionCreated", backend.diagnostics().cudaSessionCreated}, {"criticalComputeVerified", backend.diagnostics().criticalComputeVerified},
            {"cpuImageFallbackDetected", backend.diagnostics().cpuImageFallbackDetected}, {"cudaNodeCount", backend.diagnostics().cudaNodeCount},
            {"cudaConvNodeCount", backend.diagnostics().cudaConvNodeCount}, {"cudaGridSampleNodeCount", backend.diagnostics().cudaGridSampleNodeCount},
            {"cpuControlNodeCount", backend.diagnostics().cpuControlNodeCount}, {"profilePath", backend.diagnostics().profilePath.string()},
            {"cancelledRequestRejected", rejected}};
        std::ofstream out(reportPath); out << report.dump(2) << '\n'; std::cout << report.dump(2) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "inference/onnx_cuda_backend.hpp"
#include "restoration/model_pack.hpp"
#include "restoration/stdf_cuda_backend.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <process.h>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace rif = rifelove;
namespace r = rifelove::restoration;

struct Planes {
    std::array<std::vector<std::uint8_t>, 3> values;
};

struct Fixture {
    r::Request request;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Fixture makeStdfFixture(bool boundedPlanes = true) {
    Fixture fixture;
    auto& request = fixture.request;
    request.task = r::Task::deblock;
    request.placement = r::Placement::sourceRateBeforeInterpolation;
    request.inputProfile = "bt709-sdr8";
    request.centerIndex = 3;
    request.sceneBounds = r::SceneBounds{0, 6, 1};
    for (std::size_t index = 0; index < 7; ++index) {
        const int width = 64;
        const int height = 64;
        auto storage = std::make_shared<Planes>();
        rif::media::DecodedFrame frame{};
        frame.index = index;
        frame.width = width;
        frame.height = height;
        frame.timestamp = rif::Rational(static_cast<std::int64_t>(index), 30);
        frame.duration = rif::Rational(1, 30);
        frame.color = {"yuv420p", 8, "bt709", "bt709", "bt709", "tv", false, {}, {}};
        for (int plane = 0; plane < 3; ++plane) {
            const int planeWidth = plane == 0 ? width : width / 2;
            const int planeHeight = plane == 0 ? height : height / 2;
            const int stride = planeWidth + 7 + plane;
            storage->values[plane].resize(static_cast<std::size_t>(stride) * planeHeight, 0x80);
            for (int row = 0; row < planeHeight; ++row)
                for (int column = 0; column < planeWidth; ++column)
                    storage->values[plane][static_cast<std::size_t>(row) * stride + column] =
                        static_cast<std::uint8_t>(plane == 0 ? (16 + (row * 3 + column * 5 + static_cast<int>(index) * 7) % 220)
                                                            : (80 + (row + column + static_cast<int>(index) * 11 + plane * 13) % 96));
            frame.planes[plane] = {storage->values[plane].data(), stride,
                                   boundedPlanes ? std::optional<rif::media::FramePlaneExtent>{rif::media::FramePlaneExtent{
                                       storage->values[plane].data(), storage->values[plane].size(), 0}} : std::nullopt};
        }
        frame.storage = storage;
        request.context.push_back(std::move(frame));
    }
    return fixture;
}

rif::inference::GpuFrame upload(rif::gpu::CudaDevice& device, const std::vector<std::byte>& bytes, int width, int height) {
    auto buffer = device.allocate(bytes.size());
    buffer->upload(bytes);
    return {std::move(buffer), width, height};
}

std::vector<std::byte> readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "could not open RIFE fixture");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size > 0, "RIFE fixture is empty");
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(input), "could not read RIFE fixture");
    return bytes;
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 5, "usage: stdf_video_backend_check rife-model-pack runtime-directory stdf-model-pack report.json");
        const fs::path rifePackPath = fs::absolute(argv[1]);
        const fs::path runtime = fs::absolute(argv[2]);
        const fs::path stdfPackPath = fs::absolute(argv[3]);
        const fs::path reportPath = fs::absolute(argv[4]);
        fs::create_directories(reportPath.parent_path());
        const auto rifePack = rif::inference::loadModelPack(rifePackPath);
        const auto stdfPack = r::loadModelPack(stdfPackPath);
        require(r::supportsStdfCudaModel(stdfPack), "local STDF model pack did not pass strict support gate");
        auto device = std::make_shared<rif::gpu::CudaDevice>(0, 256ULL * 1024 * 1024);

        rif::inference::OnnxCudaConfig rifeConfig;
        rifeConfig.runtimeDirectory = runtime;
        rifeConfig.providerPreference = rif::inference::ProviderPreference::cudaOnly;
        rif::inference::OnnxCudaBackend rifeBackend(device, rifeConfig);
        rifeBackend.loadModel(rifePack);

        r::StdfCudaConfig stdfConfig;
        stdfConfig.runtimeDirectory = runtime;
        stdfConfig.profilePrefix = reportPath.parent_path() / ("stdf-shared-profile-" + std::to_string(::_getpid()));
        stdfConfig.maxQueuedRequests = 1;
        stdfConfig.strength = 1.0F;
        r::StdfCudaVideoRestorationBackend stdfBackend(stdfConfig);
        stdfBackend.loadModel(stdfPack);
        require(stdfBackend.capabilities().runtimeSignature.find("max-384x256") != std::string::npos,
                "STDF capabilities did not expose the qualified size ceiling");

        const auto frame0 = readFile(rifePackPath / "reference/h64-w64-t50-frame0.f32");
        const auto frame1 = readFile(rifePackPath / "reference/h64-w64-t50-frame1.f32");
        const auto first = upload(*device, frame0, 64, 64);
        const auto second = upload(*device, frame1, 64, 64);
        const auto rifeResult = rifeBackend.interpolate({first, second, 0.5F, {}, {}}).get();
        require(rifeResult.frame.width == 64 && rifeResult.frame.height == 64, "RIFE shared-runtime inference returned the wrong shape");

        auto valid = makeStdfFixture();
        const auto centerU = valid.request.context[3].planes[1].data;
        const auto centerV = valid.request.context[3].planes[2].data;
        const auto centerPts = valid.request.context[3].timestamp;
        auto result = stdfBackend.restore(valid.request).get();
        require(result.centerFrame.index == 3 && result.centerFrame.timestamp == centerPts,
                "STDF wrapper changed center index or PTS");
        require(result.centerFrame.planes[1].data == centerU && result.centerFrame.planes[2].data == centerV,
                "STDF wrapper changed center U/V plane identity");
        require(stdfBackend.capabilities().runtimeSignature.find("profile-required-before-publish-v1") != std::string::npos,
                "STDF wrapper did not publish the stable profile policy identity");
        require(stdfBackend.capabilities().fallbackReason.empty(),
                "STDF wrapper kept a pending profile diagnostic after the first verified result");

        auto noScene = makeStdfFixture();
        noScene.request.sceneBounds.reset();
        bool missingSceneRejected = false;
        try { (void)stdfBackend.restore(std::move(noScene.request)); }
        catch (const std::exception&) { missingSceneRejected = true; }
        require(missingSceneRejected, "STDF wrapper accepted a request without scene bounds");

        auto unproven = makeStdfFixture(false);
        bool unprovenRejected = false;
        try { (void)stdfBackend.restore(std::move(unproven.request)).get(); }
        catch (const std::exception&) { unprovenRejected = true; }
        require(unprovenRejected, "STDF wrapper accepted an unproven decoder plane span");

        const auto profile = stdfBackend.finishProfiling();
        require(fs::is_regular_file(profile), "STDF shared-runtime profile was not written");
        std::ofstream report(reportPath);
        report << nlohmann::json{{"passed", true}, {"rifeBackend", rifeBackend.capabilities().backend},
            {"stdfBackend", stdfBackend.capabilities().runtimeBackend},
            {"stdfRuntimeSignature", stdfBackend.capabilities().runtimeSignature},
            {"stdfProfile", profile.string()}, {"sameProcess", true},
            {"invalidSceneRejected", missingSceneRejected}, {"unprovenPlaneRejected", unprovenRejected}}
            .dump(2) << '\n';
        std::cout << "STDF/RIFE shared-runtime wrapper check passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

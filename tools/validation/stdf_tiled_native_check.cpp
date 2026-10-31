#include "restoration/stdf_cuda_backend.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace s = rifelove::validation::stdf;
namespace r = rifelove::restoration;
namespace p = r::stdf_tiles;
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
using rifelove::Rational;

namespace {

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Pixels { std::array<std::vector<std::uint8_t>, 3> planes; };

std::vector<s::BoundedPlanes> readFixture(const fs::path& inputPath, int width, int height, r::Request& request) {
    require(fs::is_regular_file(inputPath) && fs::file_size(inputPath) ==
        s::multiply(7, s::multiply(width, height)), "input-y7.u8 must contain exactly seven visible Y8 planes");
    std::ifstream input(inputPath, std::ios::binary);
    std::vector<s::BoundedPlanes> bounds;
    request.task = r::Task::deblock;
    request.inputProfile = "bt709-sdr8";
    request.centerIndex = 3;
    request.sceneBounds = r::SceneBounds{100, 106, 7};
    for (std::size_t i = 0; i < 7; ++i) {
        rifelove::media::DecodedFrame frame{};
        frame.index = 100 + i; frame.width = width; frame.height = height;
        frame.timestamp = Rational(static_cast<std::int64_t>(frame.index * 3003 + (i % 3) * 77), 90000);
        frame.duration = Rational(3003 + static_cast<std::int64_t>(i % 2) * 17, 90000);
        frame.timestampRecovered = i % 2 != 0;
        frame.color = {"yuv420p", 8, "bt709", "bt709", "bt709", "tv", false, {}, {}};
        auto storage = std::make_shared<Pixels>();
        s::BoundedPlanes extent;
        for (std::size_t plane = 0; plane < 3; ++plane) {
            const int w = plane == 0 ? width : (width + 1) / 2;
            const int h = plane == 0 ? height : (height + 1) / 2;
            const int stride = w + 3 + static_cast<int>(plane);
            auto& bytes = storage->planes[plane];
            bytes.assign(s::multiply(stride, h), 0xFA);
            for (int row = 0; row < h; ++row) {
                auto* data = bytes.data() + static_cast<std::size_t>(row) * stride;
                if (plane == 0) require(static_cast<bool>(input.read(reinterpret_cast<char*>(data), w)), "cannot read luma fixture row");
                else std::fill_n(data, w, static_cast<std::uint8_t>(64 + 31 * plane + i));
            }
            frame.planes[plane] = {bytes.data(), stride,
                rifelove::media::FramePlaneExtent{bytes.data(), bytes.size(), 0}};
            extent[plane] = bytes;
        }
        frame.storage = storage;
        request.context.push_back(std::move(frame)); bounds.push_back(extent);
    }
    require(input.peek() == EOF, "raw luma fixture contains extra bytes");
    return bounds;
}

void metadata(const rifelove::media::DecodedFrame& output, const rifelove::media::DecodedFrame& center) {
    require(output.index == center.index && output.width == center.width && output.height == center.height &&
        output.timestamp == center.timestamp && output.duration == center.duration &&
        output.timestampRecovered == center.timestampRecovered, "native tiled geometry/exact timestamp metadata changed");
    require(output.color.pixelFormat == center.color.pixelFormat && output.color.bitDepth == center.color.bitDepth &&
        output.color.primaries == center.color.primaries && output.color.transfer == center.color.transfer &&
        output.color.matrix == center.color.matrix && output.color.range == center.color.range &&
        output.color.hdrTransfer == center.color.hdrTransfer && !output.color.masteringDisplay && !output.color.contentLight,
        "native tiled color metadata changed");
    for (std::size_t plane : {1U, 2U}) {
        require(output.planes[plane].data == center.planes[plane].data && output.planes[plane].stride == center.planes[plane].stride,
            "native tiled U/V plane identity changed");
        require(output.planes[plane].extent->allocationBegin == center.planes[plane].extent->allocationBegin &&
            output.planes[plane].extent->allocationBytes == center.planes[plane].extent->allocationBytes &&
            output.planes[plane].extent->dataOffset == center.planes[plane].extent->dataOffset,
            "native tiled U/V extent identity changed");
    }
}

bool nearSeam(int coordinate, int dimension, int core) {
    const int previous = coordinate / core * core;
    const int next = previous + core;
    return (previous > 0 && coordinate - previous < 2) || (next < dimension && next - coordinate <= 2);
}

int check(const fs::path& graph, const fs::path& runtime, const fs::path& fixture, const fs::path& evidence) {
    std::ifstream manifestFile(fixture / "manifest.json", std::ios::binary);
    require(static_cast<bool>(manifestFile), "missing fixture manifest.json");
    Json manifest; manifestFile >> manifest;
    require(manifest.at("schemaVersion") == 1 && manifest.at("graphSha256") == s::graphSha256 &&
        manifest.at("tilePolicy") == p::policyId, "raw fixture schema/graph/tile policy identity mismatch");
    const int width = manifest.at("width").get<int>(), height = manifest.at("height").get<int>();
    require(p::supportsDimensions(width, height), "raw fixture exceeds native tiled visible-image policy bounds");
    const float strength = manifest.at("strength").get<float>();
    require(std::isfinite(strength) && strength > 0 && strength <= 1, "native fixture strength must be finite in (0,1]");
    const auto oracle = manifest.at("oracle").get<std::string>();
    require(oracle == "cpu-whole-frame-global-pad8" || oracle == "cpu-independent-tiled-global-pad8",
        "raw fixture must name its independent CPU reference kind");
    const auto referencePath = fixture / "reference-y.u8";
    require(fs::is_regular_file(referencePath) && fs::file_size(referencePath) == s::multiply(width, height),
        "reference-y.u8 byte count mismatch");
    require(!fs::exists(evidence), "choose a fresh native evidence directory");
    fs::create_directories(evidence);
    r::Request request;
    const auto bounds = readFixture(fixture / "input-y7.u8", width, height, request);
    auto center = request.context.at(3);
    const std::weak_ptr<const void> centerOwner = center.storage;
    const std::weak_ptr<const void> otherOwner = request.context.at(0).storage;
    r::StdfCudaConfig config;
    config.runtimeDirectory = runtime;
    config.profilePrefix = evidence / "native-cuda-profile";
    config.enableTiling = true;
    r::StdfCudaBackend backend(config);
    backend.loadModel(graph);
    const auto start = std::chrono::steady_clock::now();
    const auto output = backend.restore(request, bounds, {100, 106, 7}, strength);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    metadata(output, center);
    request.context.clear(); center.storage.reset();
    require(!centerOwner.expired() && otherOwner.expired(), "native result did not retain exactly the center source owner");
    require(output.planes[1].data[0] == 98 && output.planes[2].data[0] == 129,
        "native center U/V storage did not survive request destruction");
    std::ifstream reference(referencePath, std::ios::binary);
    std::ofstream result(evidence / "native-y.u8", std::ios::binary);
    std::vector<std::uint8_t> referenceRow(static_cast<std::size_t>(width));
    std::uint64_t absoluteSum = 0, changed = 0, seamSum = 0, seamSamples = 0;
    int maximumDifference = 0, seamMaximum = 0;
    for (int y = 0; y < height; ++y) {
        require(static_cast<bool>(reference.read(reinterpret_cast<char*>(referenceRow.data()), width)), "cannot read CPU reference row");
        const auto* row = output.planes[0].data + static_cast<std::size_t>(y) * output.planes[0].stride;
        result.write(reinterpret_cast<const char*>(row), width);
        require(static_cast<bool>(result), "cannot write native restored luma");
        for (int x = 0; x < width; ++x) {
            const int difference = std::abs(static_cast<int>(row[x]) - referenceRow[static_cast<std::size_t>(x)]);
            absoluteSum += static_cast<std::uint64_t>(difference); changed += difference != 0;
            maximumDifference = std::max(maximumDifference, difference);
            if (nearSeam(x, width, p::coreWidth) || nearSeam(y, height, p::coreHeight)) {
                seamSum += static_cast<std::uint64_t>(difference); ++seamSamples;
                seamMaximum = std::max(seamMaximum, difference);
            }
        }
    }
    result.close(); require(static_cast<bool>(result), "cannot finalize native luma artifact");
    const auto d = backend.diagnostics();
    require(d.criticalComputeVerified && !d.cpuImageFallbackDetected && d.tilingEnabled && d.tilePolicy == p::policyId,
        "native tiled CUDA/provider/policy evidence is incomplete");
    require(d.tilesPlanned == p::tileCount(width, height) && d.tilesCompleted == d.tilesPlanned &&
        d.peakHostFloatBufferBytes <= 8U * 384U * 256U * sizeof(float) &&
        d.maximumTileInputWidth <= 384 && d.maximumTileInputHeight <= 256,
        "native tiled tile-count/shape/payload diagnostics disagree with fixed policy");
    Json report{{"schemaVersion", 1}, {"case", manifest.value("case", "unnamed")}, {"graph_sha256", s::graphSha256},
        {"tile_policy", d.tilePolicy}, {"source_width", width}, {"source_height", height},
        {"global_padded_width", p::alignedDimension(width)}, {"global_padded_height", p::alignedDimension(height)},
        {"strength", strength}, {"oracle", oracle}, {"evaluation_only", true},
        {"whole_frame_oracle_present", oracle == "cpu-whole-frame-global-pad8"},
        {"cuda_fp32_error_measured", false}, {"runtime_version", d.runtimeVersion},
        {"critical_compute_verified", d.criticalComputeVerified}, {"cuda_conv_kernels", d.cudaConvNodeCount},
        {"cuda_grid_sample_kernels", d.cudaGridSampleNodeCount}, {"cpu_image_fallback", d.cpuImageFallbackDetected},
        {"tiles_planned", d.tilesPlanned}, {"tiles_completed", d.tilesCompleted},
        {"maximum_graph_input", {d.maximumTileInputWidth, d.maximumTileInputHeight}},
        {"peak_adapter_float_buffer_bytes", d.peakHostFloatBufferBytes}, {"adapter_owned_payload_bytes", d.adapterOwnedPayloadBytes},
        {"retained_plane_span_bytes", d.retainedPlaneSpanBytes}, {"payload_is_total_device_or_process_memory", false},
        {"exact_pts_duration_recovery", true}, {"center_chroma_identity_retained", true}, {"source_owners_checked", true},
        {"native_elapsed_seconds", elapsed}, {"max_8bit_difference", maximumDifference},
        {"mean_8bit_absolute_error", static_cast<double>(absoluteSum) / static_cast<double>(s::multiply(width, height))},
        {"changed_samples", changed}, {"seam_max_8bit_difference", seamMaximum},
        {"seam_mean_8bit_absolute_error", seamSamples ? static_cast<double>(seamSum) / static_cast<double>(seamSamples) : 0.0},
        {"profile_path", d.profilePath.string()}, {"passed", maximumDifference <= 1}};
    std::ofstream reportFile(evidence / "report.json", std::ios::binary);
    reportFile << report.dump(2) << '\n'; reportFile.close();
    require(static_cast<bool>(reportFile), "cannot finalize native tiled report");
    std::cout << report.dump(2) << '\n';
    return maximumDifference <= 1 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 5, "usage: rifelove-validate-stdf-tiled-native graph.onnx runtime-directory fixture-directory fresh-evidence-directory");
        return check(argv[1], argv[2], argv[3], argv[4]);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

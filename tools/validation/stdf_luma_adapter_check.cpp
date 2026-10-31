// Standalone CPU/developer checks. Not linked to, or registered by, RifeLove.
#include "stdf_luma_adapter.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <type_traits>

namespace s = rifelove::validation::stdf;
namespace r = rifelove::restoration;
using rifelove::Rational;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

struct Pixels { std::array<std::vector<std::uint8_t>, 3> planes; };
struct Fixture {
    std::string name;
    r::Request request;
    std::vector<s::BoundedPlanes> extents;
    s::SceneBounds scene;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Fixture fixture(std::string name, std::size_t first = 100, std::size_t count = 7,
                std::size_t missingBefore = 0, std::size_t missingAfter = 0,
                int width = 67, int height = 49, s::SceneBounds scene = {90, 110, 1}) {
    Fixture result;
    result.name = std::move(name); result.scene = scene;
    auto& request = result.request;
    request.task = r::Task::deblock; request.inputProfile = "bt709-sdr8";
    request.missingFramesBefore = missingBefore; request.missingFramesAfter = missingAfter;
    request.centerIndex = 3 - missingBefore;
    for (std::size_t i = 0; i < count; ++i) {
        rifelove::media::DecodedFrame frame{};
        frame.index = first + i; frame.width = width; frame.height = height;
        auto pts = [](std::size_t n) { return Rational(static_cast<std::int64_t>(n * 3003 + (n % 3) * 77), 90000); };
        frame.timestamp = pts(frame.index); frame.duration = pts(frame.index + 1) - *frame.timestamp;
        frame.timestampRecovered = (i % 2 == 1);
        frame.color = {"yuv420p", 8, "bt709", "bt709", "bt709", "tv", false, {}, {}};
        auto pixels = std::make_shared<Pixels>();
        s::BoundedPlanes bounds;
        for (int p = 0; p < 3; ++p) {
            const int w = p == 0 ? width : (width + 1) / 2;
            const int h = p == 0 ? height : (height + 1) / 2;
            const int stride = w + (p == 0 ? 5 : p == 1 ? 3 : 7);
            pixels->planes[p].assign(static_cast<std::size_t>(stride) * h, 0xFA);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                pixels->planes[p][static_cast<std::size_t>(y) * stride + x] = static_cast<std::uint8_t>(
                    p == 0 ? 16 + ((x * 7 + y * 11 + frame.index * 13 + ((x + y) % 5) * 9) % 220)
                           : 16 + ((x * 3 + y * 5 + frame.index * 17 + p * 53) % 225));
            frame.planes[p] = {pixels->planes[p].data(), stride};
            bounds[p] = pixels->planes[p];
        }
        frame.storage = pixels;
        request.context.push_back(std::move(frame)); result.extents.push_back(bounds);
    }
    return result;
}

std::vector<std::uint8_t> visible(const rifelove::media::DecodedFrame& frame, int p) {
    const int width = p == 0 ? frame.width : (frame.width + 1) / 2;
    const int height = p == 0 ? frame.height : (frame.height + 1) / 2;
    std::vector<std::uint8_t> result;
    for (int y = 0; y < height; ++y) {
        const auto* row = frame.planes[p].data + static_cast<std::size_t>(y) * frame.planes[p].stride;
        result.insert(result.end(), row, row + width);
    }
    return result;
}

void metadata(const rifelove::media::DecodedFrame& output, const rifelove::media::DecodedFrame& center) {
    require(output.index == center.index && output.width == center.width && output.height == center.height,
            "center identity/geometry changed");
    require(output.timestamp == center.timestamp && output.duration == center.duration &&
            output.timestampRecovered == center.timestampRecovered, "exact VFR PTS/duration/recovery changed");
    require(output.color.pixelFormat == center.color.pixelFormat && output.color.bitDepth == center.color.bitDepth &&
            output.color.primaries == center.color.primaries && output.color.transfer == center.color.transfer &&
            output.color.matrix == center.color.matrix && output.color.range == center.color.range &&
            output.color.hdrTransfer == center.color.hdrTransfer && !output.color.masteringDisplay && !output.color.contentLight,
            "center color metadata changed");
    for (int p = 1; p < 3; ++p) {
        require(output.planes[p].data == center.planes[p].data && output.planes[p].stride == center.planes[p].stride,
                "center chroma storage was replaced");
        require(visible(output, p) == visible(center, p), "center chroma bytes changed");
    }
}

template<class T> void binary(const fs::path& path, std::span<const T> values) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size_bytes()));
    require(static_cast<bool>(file), "cannot write fixture");
}
template<class T> void binary(const fs::path& path, const std::vector<T>& values) { binary(path, std::span<const T>(values)); }

std::vector<float> readPrediction(const fs::path& path, std::size_t count) {
    require(fs::file_size(path) == count * sizeof(float), "CPU prediction byte count mismatch");
    std::vector<float> result(count);
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(count * sizeof(float)))),
            "cannot read CPU prediction");
    return result;
}

std::vector<Fixture> fixtures() {
    std::vector<Fixture> cases;
    cases.push_back(fixture("interior-vfr-odd"));
    cases.push_back(fixture("scene-start", 200, 4, 3, 0, 65, 51, {200, 207, 2}));
    cases.push_back(fixture("scene-end", 304, 4, 0, 3, 73, 57, {300, 307, 3}));
    cases.push_back(fixture("one-frame-scene", 400, 1, 3, 3, 9, 7, {400, 400, 4}));
    cases.push_back(fixture("qualified-max-edge", 500, 7, 0, 0, 383, 255, {490, 510, 5}));
    return cases;
}

Json selftests(const s::VerifiedWeights& model, const fs::path& output) {
    Json tests = Json::array();
    auto test = [&](const std::string& name, auto body) {
        body(); tests.push_back({{"name", name}, {"passed", true}});
    };
    auto reject = [&](const std::string& name, s::Failure kind, auto modify) {
        test(name, [&] {
            auto f = fixture("reject"); s::Limits limits; modify(f, limits);
            try { (void)s::plan(f.request, f.extents, f.scene, model, limits); }
            catch (const s::Error& error) { require(error.kind == kind, "wrong rejection category"); return; }
            throw std::runtime_error("invalid request unexpectedly accepted");
        });
    };
    test("sha256-empty-and-abc", [&] {
        require(s::sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256");
        const std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        require(s::sha256(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc SHA-256");
    });
    test("wrong-weight-hash-rejected", [&] {
        const std::vector<std::uint8_t> bytes{1, 2, 3}; binary(output / "wrong-model.bin", bytes);
        try { (void)s::VerifiedWeights::load(output / "wrong-model.bin"); }
        catch (const s::Error& e) { require(e.kind == s::Failure::integrity, "wrong hash category"); return; }
        throw std::runtime_error("wrong weight identity accepted");
    });
    test("graph-byte-budget-rejected", [&] {
        try { (void)s::VerifiedWeights::load(output / "wrong-model.bin", 1); }
        catch (const s::Error& e) { require(e.kind == s::Failure::memoryBudget, "wrong graph budget category"); return; }
        throw std::runtime_error("oversize graph accepted");
    });
    test("seven-slots-code-values-padding-no-stride-leak", [&] {
        for (auto& f : fixtures()) {
            auto p = s::plan(f.request, f.extents, f.scene, model); const auto tensor = s::pack(p);
            const auto g = p.geometry();
            require(p.shape() == std::array<std::int64_t, 4>{1, 7, g[3], g[2]}, "tensor shape mismatch");
            for (int slot = 0; slot < 7; ++slot) for (int y = 0; y < g[3]; ++y) for (int x = 0; x < g[2]; ++x) {
                const auto expectedIndex = static_cast<std::size_t>(std::clamp(slot - static_cast<int>(f.request.missingFramesBefore),
                                                                             0, static_cast<int>(f.request.context.size()) - 1));
                require(p.slots()[slot] == expectedIndex, "temporal edge mapping mismatch");
                const auto& source = f.request.context[expectedIndex];
                const auto sample = source.planes[0].data[static_cast<std::size_t>(std::min(y, g[1] - 1)) * source.planes[0].stride +
                                                         std::min(x, g[0] - 1)];
                require(tensor[static_cast<std::size_t>(slot) * p.outputCount() + static_cast<std::size_t>(y) * g[2] + x] ==
                        static_cast<float>(sample) / 255.0F, "tensor normalization/edge padding incorrect");
            }
        }
    });
    test("immutable-validated-plan-retains-center-chroma-and-timing", [&] {
        auto f = fixture("lifetime"); auto center = f.request.context[f.request.centerIndex];
        const auto u = visible(center, 1), v = visible(center, 2), y = visible(center, 0);
        std::weak_ptr<const void> original = center.storage;
        auto result = [&] {
            const auto p = s::plan(f.request, f.extents, f.scene, model);
            f.request.context.clear(); f.extents.clear();
            const auto packed = s::pack(p);
            auto restored = s::complete(p, std::span<const float>(packed).subspan(3 * p.outputCount(), p.outputCount()), 1);
            metadata(restored.centerFrame, center);
            require(restored.centerFrame.planes[0].data != center.planes[0].data, "Y did not receive independent storage");
            center.storage.reset(); return restored;
        }();
        require(!original.expired(), "original center released while returned chroma is in use");
        require(visible(result.centerFrame, 0) == y && visible(result.centerFrame, 1) == u && visible(result.centerFrame, 2) == v,
                "output bytes changed after all input requests/plans released");
        result.centerFrame.storage.reset(); require(original.expired(), "center owner leaked");
    });
    test("clamp-fixed-blend-and-quantization", [&] {
        auto f = fixture("blend"); const auto p = s::plan(f.request, f.extents, f.scene, model);
        std::vector<float> prediction(p.outputCount(), 0.5F); prediction[0] = -1; prediction[1] = 2;
        const auto result = s::complete(p, prediction, 0.5F, 0.25);
        const auto y = visible(p.center(), 0), actual = visible(result.centerFrame, 0);
        require(actual[0] == static_cast<std::uint8_t>(std::lround(y[0] * 0.5F)), "lower clamp/blend");
        require(actual[1] == static_cast<std::uint8_t>(std::lround((y[1] + 255.0F) * 0.5F)), "upper clamp/blend");
        require(actual[2] == static_cast<std::uint8_t>(std::lround((y[2] + 127.5F) * 0.5F)), "half-level quantization");
        require(result.elapsedSeconds == 0.25, "elapsed inference time changed");
    });
    test("zero-strength-bypasses-inference-and-output-allocation", [&] {
        auto f = fixture("bypass"); bool called = false;
        auto result = s::run(model, f.request, f.extents, f.scene, 0, [&](auto&&...) { called = true; });
        require(!called && result.centerFrame.storage == f.request.context[f.request.centerIndex].storage,
                "zero strength unexpectedly inferred/copied");
    });
    test("run-callback-verified-bytes-shape-and-center-slot", [&] {
        auto f = fixture("dispatch"); bool called = false;
        const auto result = s::run(model, f.request, f.extents, f.scene, 1,
            [&](auto graphBytes, auto input, auto shape, auto outputTensor, auto stop) {
                called = true;
                require(graphBytes.data() == model.bytes().data() && s::sha256(graphBytes) == s::graphSha256,
                        "callback did not receive verified immutable graph bytes");
                require(shape == std::array<std::int64_t, 4>{1, 7, 56, 72} && input.size() == 7 * outputTensor.size() &&
                        !stop.stop_requested(), "dispatch shape/token contract changed");
                std::copy_n(input.begin() + 3 * outputTensor.size(), outputTensor.size(), outputTensor.begin());
            });
        require(called && visible(result.centerFrame, 0) == visible(f.request.context[f.request.centerIndex], 0),
                "identity callback did not reconstruct center Y");
        metadata(result.centerFrame, f.request.context[f.request.centerIndex]);
    });
    test("inclusive-payload-and-plane-span-budgets", [&] {
        auto f = fixture("exact-budget"); const auto p = s::plan(f.request, f.extents, f.scene, model);
        s::Limits limits; limits.maximumOwnedPayloadBytes = p.ownedBytes(); limits.maximumRetainedPlaneSpanBytes = p.retainedBytes();
        (void)s::plan(f.request, f.extents, f.scene, model, limits);
        limits.maximumOwnedPayloadBytes -= 1;
        bool payloadRejected = false;
        try { (void)s::plan(f.request, f.extents, f.scene, model, limits); }
        catch (const s::Error& e) { payloadRejected = e.kind == s::Failure::memoryBudget; }
        require(payloadRejected, "one-byte payload overflow accepted");
        limits.maximumOwnedPayloadBytes += 1; limits.maximumRetainedPlaneSpanBytes -= 1;
        bool spanRejected = false;
        try { (void)s::plan(f.request, f.extents, f.scene, model, limits); }
        catch (const s::Error& e) { spanRejected = e.kind == s::Failure::memoryBudget; }
        require(spanRejected, "one-byte retained span overflow accepted");
    });
    test("cancel-before-dispatch", [&] {
        auto f = fixture("cancel"); std::stop_source stop; stop.request_stop(); f.request.stop = stop.get_token(); bool called = false;
        try { (void)s::run(model, f.request, f.extents, f.scene, 1, [&](auto&&...) { called = true; }); }
        catch (const s::Error& e) { require(e.kind == s::Failure::cancelled && !called, "pre-cancel dispatch"); return; }
        throw std::runtime_error("pre-cancel accepted");
    });
    test("completed-inference-after-cancel-is-discarded", [&] {
        auto f = fixture("inflight"); std::stop_source stop; f.request.stop = stop.get_token(); bool called = false;
        try {
            (void)s::run(model, f.request, f.extents, f.scene, 1,
                [&](auto, auto input, auto, auto outputTensor, auto) {
                    called = true; std::copy_n(input.begin(), outputTensor.size(), outputTensor.begin()); stop.request_stop();
                });
        } catch (const s::Error& e) { require(e.kind == s::Failure::cancelled && called, "in-flight result was not discarded"); return; }
        throw std::runtime_error("cancelled success was published");
    });
    test("inference-errors-propagate-without-output", [&] {
        auto f = fixture("runtime-error");
        try { (void)s::run(model, f.request, f.extents, f.scene, 1, [&](auto&&...) { throw std::runtime_error("injected native Run error"); }); }
        catch (const std::runtime_error& e) { require(std::string(e.what()) == "injected native Run error", "inference error hidden"); return; }
        throw std::runtime_error("native error was swallowed");
    });
    test("unwritten-inference-output-is-not-silent-black", [&] {
        auto f = fixture("incomplete-runtime");
        try { (void)s::run(model, f.request, f.extents, f.scene, 1, [&](auto&&...) {}); }
        catch (const s::Error& e) { require(e.kind == s::Failure::nonFiniteOutput, "incomplete output rejection category"); return; }
        throw std::runtime_error("callback without output silently published black");
    });
    for (int mode = 0; mode < 6; ++mode) test("output-rejection-" + std::to_string(mode), [&] {
        auto f = fixture("bad-output"); const auto p = s::plan(f.request, f.extents, f.scene, model);
        std::vector<float> prediction(p.outputCount(), 0.5F); float strength = 1; double elapsed = 0;
        if (mode == 0) prediction.pop_back();
        if (mode == 1) prediction.back() = std::numeric_limits<float>::quiet_NaN();
        if (mode == 2) prediction[0] = std::numeric_limits<float>::infinity();
        if (mode == 3) strength = -0.1F;
        if (mode == 4) strength = std::numeric_limits<float>::quiet_NaN();
        if (mode == 5) elapsed = -1;
        try { (void)s::complete(p, prediction, strength, elapsed); }
        catch (const s::Error& e) { require(e.kind == (mode == 1 || mode == 2 ? s::Failure::nonFiniteOutput : s::Failure::invalidRequest),
                                          "wrong output rejection category"); return; }
        throw std::runtime_error("invalid output accepted");
    });
    reject("wrong-task", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.task = r::Task::denoise; });
    reject("wrong-placement", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.placement = r::Placement::outputRateAfterInterpolation; });
    reject("unknown-profile", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.inputProfile.clear(); });
    reject("wrong-center", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.centerIndex = 0; });
    reject("too-many-missing-slots", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.missingFramesBefore = 4; });
    reject("short-real-context", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context.pop_back(); f.extents.pop_back(); });
    reject("cross-cut-context", s::Failure::invalidRequest, [](auto& f, auto&) { f.scene.lastIndex = 103; });
    reject("inverted-scene-bounds", s::Failure::invalidRequest, [](auto& f, auto&) { f.scene.firstIndex = 111; });
    reject("invented-history-inside-scene", s::Failure::invalidRequest, [](auto& f, auto&) {
        f.request.context.erase(f.request.context.begin()); f.extents.erase(f.extents.begin());
        f.request.missingFramesBefore = 1; f.request.centerIndex = 2;
    });
    reject("invented-future-before-scene-end", s::Failure::invalidRequest, [](auto& f, auto&) {
        f.request.context.pop_back(); f.extents.pop_back(); f.request.missingFramesAfter = 1;
    });
    reject("unknown-future-boundary", s::Failure::invalidRequest, [](auto& f, auto&) {
        f.request.context.pop_back(); f.extents.pop_back(); f.request.missingFramesAfter = 1; f.scene.lastIndex.reset();
    });
    reject("nonmonotonic-pts", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[2].timestamp = f.request.context[1].timestamp; });
    reject("nonadjacent-index", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[2].index += 1; });
    reject("missing-pts", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].timestamp.reset(); });
    reject("invalid-duration", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].duration = Rational(0); });
    reject("unknown-color", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.primaries.clear(); });
    reject("unqualified-bt470bg", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.primaries = "bt470bg"; });
    reject("unqualified-full-range", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.range = "pc"; });
    reject("unqualified-nv12", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.pixelFormat = "nv12"; });
    reject("unqualified-ten-bit", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.bitDepth = 10; });
    reject("hdr-transfer", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.hdrTransfer = true; });
    reject("hdr-mastering-data", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.masteringDisplay.emplace(); });
    reject("hdr-content-light", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].color.contentLight.emplace(); });
    reject("geometry-mismatch", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].width -= 1; });
    reject("unowned-frame", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].storage.reset(); });
    reject("negative-stride", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].planes[0].stride = -72; });
    reject("short-chroma-stride", s::Failure::invalidRequest, [](auto& f, auto&) { f.request.context[1].planes[1].stride = 1; });
    reject("different-bounded-pointer", s::Failure::invalidRequest, [](auto& f, auto&) { f.extents[1][0] = f.extents[1][0].subspan(1); });
    reject("short-plane-buffer", s::Failure::invalidRequest, [](auto& f, auto&) { f.extents[1][2] = f.extents[1][2].first(3); });
    reject("over-qualified-width", s::Failure::memoryBudget, [](auto& f, auto&) { f.request.context[3].width = 385; });
    reject("caller-cannot-widen-shape-qualification", s::Failure::memoryBudget, [](auto& f, auto& l) {
        f.request.context[3].width = 385; l.maximumWidth = 1024;
    });
    reject("adapter-payload-budget", s::Failure::memoryBudget, [](auto&, auto& l) { l.maximumOwnedPayloadBytes = 1024; });
    reject("retained-plane-span-budget", s::Failure::memoryBudget, [](auto&, auto& l) { l.maximumRetainedPlaneSpanBytes = 1024; });
    reject("signed-padding-overflow", s::Failure::memoryBudget, [](auto& f, auto& l) {
        f.request.context[3].width = std::numeric_limits<int>::max(); l.maximumWidth = std::numeric_limits<int>::max();
    });
    test("checked-size-arithmetic", [&] {
        int rejected = 0;
        try { (void)s::multiply(std::numeric_limits<std::size_t>::max(), 2); } catch (const s::Error& e) { rejected += e.kind == s::Failure::memoryBudget; }
        try { (void)s::add(std::numeric_limits<std::size_t>::max(), 1); } catch (const s::Error& e) { rejected += e.kind == s::Failure::memoryBudget; }
        require(rejected == 2, "size overflow not rejected");
    });
    return tests;
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: stdf_luma_adapter_check prepare|complete graph.onnx evidence-directory");
        const std::string mode = argv[1]; const fs::path graph = argv[2], output = argv[3];
        require(mode == "prepare" || mode == "complete", "unknown mode");
        if (mode == "prepare") require(!fs::exists(output), "choose a fresh evidence directory");
        fs::create_directories(output);
        const auto model = s::VerifiedWeights::load(graph);
        Json report{{"schemaVersion", 1}, {"graph_sha256", s::sha256(model.bytes())}, {"mode", mode}, {"cases", Json::array()}};
        if (mode == "prepare") report["selftests"] = selftests(model, output);
        for (auto& f : fixtures()) {
            const auto p = s::plan(f.request, f.extents, f.scene, model); const auto g = p.geometry();
            const auto folder = output / f.name;
            Json item{{"name", f.name}, {"shape", p.shape()}, {"source_size", {g[0], g[1]}}, {"context_slots", p.slots()},
                      {"center_index", p.center().index}, {"center_pts", p.center().timestamp->toString()},
                      {"center_duration", p.center().duration->toString()}, {"timestamp_recovered", p.center().timestampRecovered},
                      {"owned_payload_bytes", p.ownedBytes()}, {"retained_plane_span_bytes", p.retainedBytes()}};
            if (mode == "prepare") {
                fs::create_directories(folder); const auto tensor = s::pack(p); binary(folder / "input.f32", tensor);
                for (int plane = 0; plane < 3; ++plane) binary(folder / ("center-" + std::to_string(plane) + ".u8"), visible(p.center(), plane));
                // Write visible real input planes independently for a Python packing oracle.
                for (std::size_t i = 0; i < f.request.context.size(); ++i)
                    binary(folder / ("real-" + std::to_string(i) + "-y.u8"), visible(f.request.context[i], 0));
            } else {
                const auto prediction = readPrediction(folder / "prediction.f32", p.outputCount());
                auto restored = s::complete(p, prediction, 1, 0); metadata(restored.centerFrame, p.center());
                std::stop_source stopped;
                f.request.stop = stopped.get_token();
                const auto cancelledPlan = s::plan(f.request, f.extents, f.scene, model);
                stopped.request_stop();
                bool discarded = false;
                try { (void)s::complete(cancelledPlan, prediction, 1, 0); }
                catch (const s::Error& e) { discarded = e.kind == s::Failure::cancelled; }
                require(discarded, "completed real-model prediction was published after cancellation");
                std::size_t changed = 0; const auto y = visible(restored.centerFrame, 0), original = visible(p.center(), 0);
                for (std::size_t i = 0; i < y.size(); ++i) changed += y[i] != original[i];
                for (int plane = 0; plane < 3; ++plane) binary(folder / ("output-" + std::to_string(plane) + ".u8"), visible(restored.centerFrame, plane));
                item["center_metadata_exact"] = true; item["center_chroma_unchanged"] = true; item["changed_y_samples"] = changed;
                item["real_prediction_discarded_after_cancel"] = true;
            }
            report["cases"].push_back(item);
        }
        report["passed"] = true;
        std::ofstream file(output / (mode + ".json")); file << report.dump(2) << '\n';
        std::cout << report.dump(2) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

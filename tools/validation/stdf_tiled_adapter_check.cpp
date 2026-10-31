// CPU contract checker. Uses verified real graph bytes but deliberately fake
// inference; this establishes packing/ownership/budget/error behavior, not
// neural-network quality or CUDA qualification. No GPU/runtime is loaded.
#include "restoration/stdf_tiled_adapter.hpp"

#include <iostream>
#include <type_traits>

namespace s = rifelove::validation::stdf;
namespace t = s::tiled;
namespace r = rifelove::restoration;
namespace p = r::stdf_tiles;
using rifelove::Rational;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Pixels { std::array<std::vector<std::uint8_t>, 3> planes; };
struct Fixture {
    r::Request request;
    std::vector<s::BoundedPlanes> extents;
    s::SceneBounds scene{100, 106, 4};
};

std::uint8_t code(int x, int y, std::size_t index, int plane = 0) {
    return static_cast<std::uint8_t>(16 + ((x * 7 + y * 11 + index * 13 + plane * 19) % 220));
}

Fixture fixture(int width = 523, int height = 275, bool singleFrame = false) {
    Fixture f;
    f.request.task = r::Task::deblock;
    f.request.inputProfile = "bt709-sdr8";
    f.request.centerIndex = singleFrame ? 0 : 3;
    f.request.missingFramesBefore = singleFrame ? 3 : 0;
    f.request.missingFramesAfter = singleFrame ? 3 : 0;
    if (singleFrame) f.scene.lastIndex = 100;
    for (std::size_t i = 0; i < (singleFrame ? 1U : 7U); ++i) {
        rifelove::media::DecodedFrame frame;
        frame.index = 100 + i;
        frame.width = width; frame.height = height;
        frame.timestamp = Rational(static_cast<std::int64_t>(frame.index * 3003 + (frame.index % 3) * 77), 90000);
        frame.duration = Rational(3003 + static_cast<std::int64_t>(frame.index % 2) * 17, 90000);
        frame.timestampRecovered = i % 2 != 0;
        frame.color = {"yuv420p", 8, "bt709", "bt709", "bt709", "tv", false, {}, {}};
        auto storage = std::make_shared<Pixels>();
        s::BoundedPlanes planes;
        for (int plane = 0; plane < 3; ++plane) {
            const int w = plane == 0 ? width : (width + 1) / 2;
            const int h = plane == 0 ? height : (height + 1) / 2;
            const int stride = w + 3 + plane;
            auto& bytes = storage->planes[plane];
            bytes.assign(static_cast<std::size_t>(stride) * h, 0xFA);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                bytes[static_cast<std::size_t>(y) * stride + x] = code(x, y, frame.index, plane);
            frame.planes[plane] = {bytes.data(), stride,
                rifelove::media::FramePlaneExtent{bytes.data(), bytes.size(), 0}};
            planes[plane] = bytes;
        }
        frame.storage = storage;
        f.request.context.push_back(std::move(frame));
        f.extents.push_back(planes);
    }
    return f;
}

void metadata(const rifelove::media::DecodedFrame& actual, const rifelove::media::DecodedFrame& expected) {
    require(actual.index == expected.index && actual.width == expected.width && actual.height == expected.height,
            "tiled output identity/size changed");
    require(actual.timestamp == expected.timestamp && actual.duration == expected.duration &&
        actual.timestampRecovered == expected.timestampRecovered, "tiled exact VFR timing/recovery changed");
    require(actual.color.pixelFormat == expected.color.pixelFormat && actual.color.bitDepth == expected.color.bitDepth &&
        actual.color.primaries == expected.color.primaries && actual.color.transfer == expected.color.transfer &&
        actual.color.matrix == expected.color.matrix && actual.color.range == expected.color.range &&
        actual.color.hdrTransfer == expected.color.hdrTransfer && !actual.color.masteringDisplay && !actual.color.contentLight,
        "tiled center color metadata changed");
    for (std::size_t plane : {1U, 2U}) {
        require(actual.planes[plane].data == expected.planes[plane].data && actual.planes[plane].stride == expected.planes[plane].stride,
                "tiled center U/V identity changed");
        require(actual.planes[plane].extent->allocationBegin == expected.planes[plane].extent->allocationBegin &&
            actual.planes[plane].extent->allocationBytes == expected.planes[plane].extent->allocationBytes &&
            actual.planes[plane].extent->dataOffset == expected.planes[plane].extent->dataOffset,
            "tiled center U/V extent changed");
    }
}

template<class Callback> void rejects(s::Failure expected, Callback callback) {
    try { callback(); }
    catch (const s::Error& error) { require(error.kind == expected, "wrong tiled failure category"); return; }
    throw std::runtime_error("invalid tiled request/output was accepted");
}

void contractTests(const s::VerifiedWeights& model) {
    std::size_t count = 0;
    auto test = [&](const char* name, auto&& body) { body(); ++count; std::cout << "PASS " << name << '\n'; };
    test("global-pad-pack-all-tiles-and-exact-core-coverage", [&] {
        for (const auto dimensions : {std::array{523, 275}, std::array{854, 480}, std::array{1918, 1078},
                                     std::array{1, 1}, std::array{384, 256},
                                     std::array{3840, 8}, std::array{3839, 7}, std::array{8, 3840}}) {
            auto f = fixture(dimensions[0], dimensions[1]);
            s::Limits limits;
            limits.maximumRetainedPlaneSpanBytes = 128U * 1024U * 1024U;
            const auto plan = t::plan(f.request, f.extents, f.scene, model, limits);
            std::size_t calls = 0;
            t::Statistics stats;
            auto restored = t::run(model, plan, 1.0F,
                [&](auto graph, auto input, auto shape, auto output, auto) {
                    const auto tile = plan.tile(calls++);
                    const auto pixels = static_cast<std::size_t>(tile.inputWidth) * tile.inputHeight;
                    require(graph.size() == model.bytes().size(), "unverified graph bytes passed to inference");
                    require(shape == t::shape(tile) && input.size() == 7 * pixels && output.size() == pixels,
                            "tiled input/output shape/count mismatch");
                    for (std::size_t slot = 0; slot < 7; ++slot) {
                        const auto sourceIndex = plan.context().frames[plan.context().slots[slot]].index;
                        for (int row = 0; row < tile.inputHeight; ++row) for (int column = 0; column < tile.inputWidth; ++column) {
                            const auto x = std::min(tile.inputX + column, plan.center().width - 1);
                            const auto y = std::min(tile.inputY + row, plan.center().height - 1);
                            require(input[slot * pixels + static_cast<std::size_t>(row) * tile.inputWidth + column] ==
                                static_cast<float>(code(x, y, sourceIndex)) / 255.0F, "global pad/stride/context packing mismatch");
                        }
                    }
                    for (std::size_t i = 0; i < pixels; ++i) output[i] = 1.0F - input[3 * pixels + i];
                }, &stats);
            require(calls == p::tileCount(dimensions[0], dimensions[1]) && stats.tilesCompleted == calls,
                    "not every planned tile completed");
            require(stats.peakFloatBufferBytes <= 8U * 384U * 256U * sizeof(float), "float tensors grew with source image");
            metadata(restored.centerFrame, plan.center());
            f = {}; // Owners still live through the immutable plan and result.
            const auto& output = restored.centerFrame;
            for (int y = 0; y < output.height; ++y) for (int x = 0; x < output.width; ++x)
                require(output.planes[0].data[static_cast<std::size_t>(y) * output.planes[0].stride + x] ==
                    255 - code(x, y, output.index), "overlap/core gap or blend conversion mismatch");
        }
    });
    test("landscape-portrait-4k-plan-and-zero-short-circuit", [&] {
        for (const auto dimensions : {std::array{3840, 2160}, std::array{2160, 3840}}) {
            auto f = fixture(dimensions[0], dimensions[1], true);
            auto limits = s::Limits{};
            limits.maximumOwnedPayloadBytes = 16U * 1024U * 1024U;
            const auto plan = t::plan(f.request, f.extents, f.scene, model, limits);
            require(plan.statistics().peakFloatBufferBytes == 8U * 384U * 256U * sizeof(float), "4K float-buffer proof mismatch");
            bool called = false;
            const auto result = t::run(model, plan, 0, [&](auto&&...) { called = true; });
            require(!called && result.centerFrame.storage == plan.center().storage, "strength zero did not preserve original owner");
            require(result.centerFrame.planes[0].data == plan.center().planes[0].data, "strength zero changed original Y identity");
            metadata(result.centerFrame, plan.center());
        }
    });
    test("old-full-frame-padding-and-ceiling-remain-unchanged", [&] {
        auto f = fixture(67, 49);
        const auto full = s::plan(f.request, f.extents, f.scene, model);
        require(full.geometry() == std::array<int, 4>{67, 49, 72, 56} &&
            full.shape() == std::array<std::int64_t, 4>{1, 7, 56, 72}, "old ceil8 whole-frame plan changed");
        const auto oldTensor = s::pack(full);
        const auto tilePlan = t::plan(f.request, f.extents, f.scene, model);
        std::vector<float> tileTensor(7U * tilePlan.maximumTilePixels());
        t::packTile(tilePlan, 0, tileTensor);
        require(oldTensor == tileTensor, "single-tile global pad no longer matches old full-frame packing");
        f.request.context[3].width = 385;
        s::Limits wider; wider.maximumWidth = 3840;
        rejects(s::Failure::memoryBudget, [&] { (void)s::plan(f.request, f.extents, f.scene, model, wider); });
    });
    test("exact-byte-budgets-before-inference", [&] {
        auto f = fixture();
        const auto plan = t::plan(f.request, f.extents, f.scene, model);
        s::Limits limits;
        limits.maximumOwnedPayloadBytes = plan.statistics().ownedPayloadBytes;
        limits.maximumRetainedPlaneSpanBytes = plan.statistics().retainedPlaneSpanBytes;
        (void)t::plan(f.request, f.extents, f.scene, model, limits);
        --limits.maximumOwnedPayloadBytes;
        bool called = false;
        rejects(s::Failure::memoryBudget, [&] { (void)t::run(model, f.request, f.extents, f.scene, 1,
            [&](auto&&...) { called = true; }, limits); });
        require(!called, "budget rejection reached inference");
        ++limits.maximumOwnedPayloadBytes; --limits.maximumRetainedPlaneSpanBytes;
        rejects(s::Failure::memoryBudget, [&] { (void)t::plan(f.request, f.extents, f.scene, model, limits); });
        limits.maximumRetainedPlaneSpanBytes = plan.statistics().retainedPlaneSpanBytes;
        limits.maximumWidth = 320;
        rejects(s::Failure::memoryBudget, [&] { (void)t::plan(f.request, f.extents, f.scene, model, limits); });
    });
    test("zero-strength-still-validates-color-spans-scenes-and-budget", [&] {
        for (int mode = 0; mode < 8; ++mode) {
            auto f = fixture(); s::Limits limits;
            if (mode == 0) f.request.context[0].color.transfer = "smpte2084";
            if (mode == 1) f.extents[0][2] = f.extents[0][2].first(1);
            if (mode == 2) f.scene.lastIndex = 105;
            if (mode == 3) f.request.context[2].timestamp = f.request.context[1].timestamp;
            if (mode == 4) f.request.context[0].storage.reset();
            if (mode == 5) limits.maximumOwnedPayloadBytes = 0;
            if (mode == 6) f.request.context[3].width = 3841;
            if (mode == 7) { f.request.context[3].width = 3840; f.request.context[3].height = 2161; }
            bool called = false;
            rejects(mode >= 5 ? s::Failure::memoryBudget : s::Failure::invalidRequest, [&] {
                (void)t::run(model, f.request, f.extents, f.scene, 0, [&](auto&&...) { called = true; }, limits);
            });
            require(!called, "zero-strength validation reached inference");
        }
    });
    test("temporal-edge-padding-only-at-declared-scene-boundaries", [&] {
        auto f = fixture(264, 136, true);
        const auto plan = t::plan(f.request, f.extents, f.scene, model);
        require(std::all_of(plan.context().slots.begin(), plan.context().slots.end(), [](auto i) { return i == 0; }),
                "single-frame temporal replication mismatch");
        f.scene.firstIndex = 99;
        rejects(s::Failure::invalidRequest, [&] { (void)t::plan(f.request, f.extents, f.scene, model); });
        f.scene.firstIndex = 100; f.scene.lastIndex.reset();
        rejects(s::Failure::invalidRequest, [&] { (void)t::plan(f.request, f.extents, f.scene, model); });
    });
    test("invalid-strength-never-dispatches", [&] {
        auto f = fixture();
        for (const auto strength : {-0.1F, 1.1F, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            bool called = false;
            rejects(s::Failure::invalidRequest, [&] { (void)t::run(model, f.request, f.extents, f.scene, strength,
                [&](auto&&...) { called = true; }); });
            require(!called, "invalid strength reached inference");
        }
    });
    test("partial-nonfinite-and-failed-later-tile-discard-entire-result", [&] {
        for (int mode = 0; mode < 4; ++mode) {
            auto f = fixture(); std::size_t calls = 0; t::Statistics stats;
            try {
                (void)t::run(model, f.request, f.extents, f.scene, 1,
                    [&](auto, auto input, auto, auto output, auto) {
                        if (++calls == 2) {
                            if (mode == 0) return; // Unwritten output is NaN again on each tile.
                            if (mode == 3) throw std::runtime_error("injected second tile runtime failure");
                        }
                        std::copy_n(input.begin(), output.size(), output.begin());
                        if (calls == 2) output.back() = mode == 1 ? std::numeric_limits<float>::quiet_NaN() :
                            std::numeric_limits<float>::infinity();
                    }, {}, &stats);
            } catch (const s::Error& error) {
                require(mode != 3 && error.kind == s::Failure::nonFiniteOutput, "non-finite tile error was hidden");
                require(stats.tilesCompleted == 1 && calls == 2, "partial result progress mismatch");
                continue;
            } catch (const std::runtime_error& error) {
                require(mode == 3 && std::string(error.what()) == "injected second tile runtime failure", "runtime failure was hidden");
                require(stats.tilesCompleted == 1 && calls == 2, "failed tile progress mismatch");
                continue;
            }
            throw std::runtime_error("incomplete multi-tile result was published");
        }
    });
    test("cancel-before-pack-and-after-later-inference-discards-result", [&] {
        auto f = fixture(); std::stop_source stop; f.request.stop = stop.get_token();
        const auto plan = t::plan(f.request, f.extents, f.scene, model);
        stop.request_stop();
        std::vector<float> tensor(7U * plan.maximumTilePixels());
        rejects(s::Failure::cancelled, [&] { t::packTile(plan, 0, std::span<float>(tensor).first(
            7U * static_cast<std::size_t>(plan.tile(0).inputWidth) * plan.tile(0).inputHeight)); });
        rejects(s::Failure::cancelled, [&] { (void)t::run(model, plan, 0, [&](auto&&...) {}); });
        auto active = fixture(); std::stop_source inflight; active.request.stop = inflight.get_token();
        std::size_t calls = 0; t::Statistics stats;
        rejects(s::Failure::cancelled, [&] {
            (void)t::run(model, active.request, active.extents, active.scene, 1,
                [&](auto, auto input, auto, auto output, auto) {
                    std::copy_n(input.begin(), output.size(), output.begin());
                    if (++calls == 2) inflight.request_stop();
                }, {}, &stats);
        });
        require(calls == 2 && stats.tilesCompleted == 1, "cancelled successful inference published a core");
    });
    test("quarter-strength-blend-retains-center-and-drops-all-other-input-owners", [&] {
        auto f = fixture();
        auto source = f.request.context[3];
        const std::weak_ptr<const void> centerOwner = source.storage;
        const std::weak_ptr<const void> otherOwner = f.request.context[0].storage;
        const auto result = t::run(model, f.request, f.extents, f.scene, 0.25F,
            [](auto, auto, auto, auto output, auto) { std::fill(output.begin(), output.end(), 1.0F); });
        f = {};
        source.storage.reset();
        require(!centerOwner.expired() && otherOwner.expired(), "result retained unrelated temporal frames");
        metadata(result.centerFrame, source);
        for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
            const auto original = source.planes[0].data[static_cast<std::size_t>(y) * source.planes[0].stride + x];
            require(result.centerFrame.planes[0].data[static_cast<std::size_t>(y) * source.width + x] ==
                std::lround(original + 0.25F * (255 - original)), "explicit restoration strength was not applied");
        }
    });
    std::cout << "PASS " << count << " tiled adapter contract groups; inference was fake, no CUDA quality claim\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rifelove-test-stdf-tiled-adapter graph.onnx");
        const auto model = s::VerifiedWeights::load(argv[1]);
        contractTests(model);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#pragma once

#include "media/media_engine.hpp"

#include <future>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <vector>

namespace rifelove::restoration {

enum class Task { denoise, deblur, deblock, superResolution };
enum class Placement { sourceRateBeforeInterpolation, outputRateAfterInterpolation };

// Capabilities are attached to a verified local model descriptor. This contract
// is deliberately separate from RIFE's adjacent-frame/timestep interface.
struct Capabilities {
    std::string modelId;
    std::string adapterId;
    int adapterVersion = 0;
    std::vector<Task> tasks;
    std::vector<Placement> placements;
    std::vector<std::string> supportedInputProfiles;
    std::size_t temporalFramesBefore = 0;
    std::size_t temporalFramesAfter = 0;
    int scaleNumerator = 1;
    int scaleDenominator = 1;
    bool supportsBoundaryPadding = false;
    // Runtime-selected provider diagnostics. These are populated by a backend,
    // never trusted from a local model descriptor.
    std::string runtimeBackend;
    std::string fallbackReason;
    // Backend version and tile-policy identity for safe checkpoint reuse.
    std::string runtimeSignature;
};

struct Request {
    Task task = Task::denoise;
    Placement placement = Placement::sourceRateBeforeInterpolation;
    // Only real, timestamped decoded frames are passed. Missing clip/scene-edge
    // context is represented explicitly rather than by fake timestamps.
    std::vector<media::DecodedFrame> context;
    std::size_t centerIndex = 0;
    std::size_t missingFramesBefore = 0;
    std::size_t missingFramesAfter = 0;
    std::string inputProfile;
    std::stop_token stop;
};

struct Result {
    media::DecodedFrame centerFrame;
    double elapsedSeconds = 0.0;
};

class IVideoRestorationBackend {
public:
    virtual ~IVideoRestorationBackend() = default;
    [[nodiscard]] virtual Capabilities capabilities() const = 0;
    [[nodiscard]] virtual std::future<Result> restore(Request request) = 0;
};

class Registry final {
public:
    void registerBackend(std::shared_ptr<IVideoRestorationBackend> backend);
    [[nodiscard]] std::shared_ptr<IVideoRestorationBackend> resolve(
        const std::string& modelId, const std::string& adapterId, int adapterVersion) const;
    [[nodiscard]] std::size_t size() const noexcept { return backends_.size(); }
private:
    using Key = std::tuple<std::string,std::string,int>;
    std::map<Key,std::shared_ptr<IVideoRestorationBackend>> backends_;
};

void validateCapabilities(const Capabilities& capabilities);
// Return the task supported by the current single-frame output-rate image
// restoration path (spatial SR or deblur), otherwise no task.
[[nodiscard]] std::optional<Task> outputRateImageTask(const Capabilities& capabilities);
void validateRequest(const Capabilities& capabilities, const Request& request);
void validateResult(const Capabilities& capabilities, const Request& request, const Result& result);

} // namespace rifelove::restoration

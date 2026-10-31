#include "restoration/video_restoration_backend.hpp"
#include "media/rife_color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

namespace rifelove::restoration {
namespace {
bool safeToken(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}
bool validTask(Task task) {
    switch (task) {
        case Task::denoise: case Task::deblur: case Task::deblock: case Task::superResolution: return true;
    }
    return false;
}
bool validPlacement(Placement placement) {
    switch (placement) {
        case Placement::sourceRateBeforeInterpolation: case Placement::outputRateAfterInterpolation: return true;
    }
    return false;
}
bool hasPixels(const media::DecodedFrame& frame) {
    if (!frame.storage || frame.width <= 0 || frame.height <= 0) return false;
    return std::any_of(frame.planes.begin(),frame.planes.end(),[](const auto& plane) { return plane.data != nullptr; });
}
bool sameColor(const media::ColorInfo& left, const media::ColorInfo& right) {
    if (left.pixelFormat != right.pixelFormat || left.bitDepth != right.bitDepth || left.primaries != right.primaries ||
        left.transfer != right.transfer || left.matrix != right.matrix || left.range != right.range || left.hdrTransfer != right.hdrTransfer ||
        left.masteringDisplay.has_value() != right.masteringDisplay.has_value() || left.contentLight.has_value() != right.contentLight.has_value()) return false;
    if (left.masteringDisplay) {
        const auto& a = *left.masteringDisplay;
        const auto& b = *right.masteringDisplay;
        if (a.primaries != b.primaries || a.whitePoint != b.whitePoint || a.minLuminance != b.minLuminance || a.maxLuminance != b.maxLuminance) return false;
    }
    if (left.contentLight) {
        if (left.contentLight->maxContentLight != right.contentLight->maxContentLight ||
            left.contentLight->maxFrameAverageLight != right.contentLight->maxFrameAverageLight) return false;
    }
    return true;
}
void validateFrame(const media::DecodedFrame& frame, const char* role) {
    if (!frame.timestamp || !hasPixels(frame)) throw std::invalid_argument(std::string("Video restoration ") + role + " frame needs owned pixels and an exact timestamp");
}
void validateSceneBounds(const Request& request) {
    if (!request.sceneBounds) return;
    const auto& bounds = *request.sceneBounds;
    if (bounds.serial == 0 || (bounds.lastIndex && *bounds.lastIndex < bounds.firstIndex))
        throw std::invalid_argument("Video restoration scene bounds are invalid");
    if (request.context.empty()) throw std::invalid_argument("Video restoration scene bounds need a non-empty context");
    for (const auto& frame : request.context) {
        if (frame.index < bounds.firstIndex || (bounds.lastIndex && frame.index > *bounds.lastIndex))
            throw std::invalid_argument("Video restoration context crosses the declared scene bounds");
    }
    if (request.missingFramesBefore && request.context.front().index != bounds.firstIndex)
        throw std::invalid_argument("Video restoration cannot pad history inside a scene");
    if (request.missingFramesAfter && (!bounds.lastIndex || request.context.back().index != *bounds.lastIndex))
        throw std::invalid_argument("Video restoration future padding needs a declared scene end");
}
}

void validateCapabilities(const Capabilities& capabilities) {
    if (!safeToken(capabilities.modelId) || !safeToken(capabilities.adapterId) || capabilities.adapterVersion <= 0)
        throw std::invalid_argument("Video restoration model and adapter identity are invalid");
    if (capabilities.tasks.empty() || capabilities.placements.empty() || capabilities.supportedInputProfiles.empty())
        throw std::invalid_argument("Video restoration capabilities must declare tasks, placements and input profiles");
    if (capabilities.temporalFramesBefore > 16 || capabilities.temporalFramesAfter > 16)
        throw std::invalid_argument("Video restoration temporal context exceeds the supported bound");
    if (capabilities.scaleNumerator <= 0 || capabilities.scaleDenominator <= 0 ||
        static_cast<std::int64_t>(capabilities.scaleNumerator) > 8LL * capabilities.scaleDenominator ||
        static_cast<std::int64_t>(capabilities.scaleDenominator) > 8LL * capabilities.scaleNumerator)
        throw std::invalid_argument("Video restoration scale must be a positive ratio no larger than 8x");

    std::set<Task> tasks;
    for (const auto task : capabilities.tasks)
        if (!validTask(task) || !tasks.insert(task).second) throw std::invalid_argument("Video restoration task is invalid or duplicated");
    std::set<Placement> placements;
    for (const auto placement : capabilities.placements)
        if (!validPlacement(placement) || !placements.insert(placement).second) throw std::invalid_argument("Video restoration placement is invalid or duplicated");
    std::set<std::string> profiles;
    for (const auto& profile : capabilities.supportedInputProfiles)
        if (!safeToken(profile) || !profiles.insert(profile).second) throw std::invalid_argument("Video restoration input profile is invalid or duplicated");
    if (!tasks.contains(Task::superResolution) && capabilities.scaleNumerator != capabilities.scaleDenominator)
        throw std::invalid_argument("A non-super-resolution model must preserve its spatial scale");
    if (tasks.contains(Task::superResolution) && capabilities.scaleNumerator <= capabilities.scaleDenominator)
        throw std::invalid_argument("A super-resolution model must declare an upscale ratio");
}

std::optional<Task> outputRateImageTask(const Capabilities& capabilities) {
    if (capabilities.tasks.size() != 1 || capabilities.temporalFramesBefore != 0 || capabilities.temporalFramesAfter != 0 ||
        std::find(capabilities.placements.begin(),capabilities.placements.end(),Placement::outputRateAfterInterpolation) == capabilities.placements.end() ||
        capabilities.scaleDenominator != 1)
        return std::nullopt;
    const auto task = capabilities.tasks.front();
    if (task == Task::superResolution && capabilities.scaleNumerator >= 2 && capabilities.scaleNumerator <= 4) return task;
    if (task == Task::deblur && capabilities.scaleNumerator == 1) return task;
    return std::nullopt;
}

void validateRequest(const Capabilities& capabilities, const Request& request) {
    validateCapabilities(capabilities);
    if (request.stop.stop_requested()) throw std::runtime_error("Video restoration request was cancelled");
    if (!validTask(request.task) || std::find(capabilities.tasks.begin(),capabilities.tasks.end(),request.task) == capabilities.tasks.end())
        throw std::invalid_argument("Video restoration model does not support the requested task");
    if (!validPlacement(request.placement) || std::find(capabilities.placements.begin(),capabilities.placements.end(),request.placement) == capabilities.placements.end())
        throw std::invalid_argument("Video restoration model does not support the requested pipeline placement");
    if (std::find(capabilities.supportedInputProfiles.begin(),capabilities.supportedInputProfiles.end(),request.inputProfile) == capabilities.supportedInputProfiles.end())
        throw std::invalid_argument("Video restoration model does not support the input color profile");
    if (request.missingFramesBefore > capabilities.temporalFramesBefore || request.missingFramesAfter > capabilities.temporalFramesAfter)
        throw std::invalid_argument("Video restoration request declares more missing context than the model window permits");
    if ((request.missingFramesBefore || request.missingFramesAfter) && !capabilities.supportsBoundaryPadding)
        throw std::invalid_argument("Video restoration model does not support clip or scene-boundary padding");
    const auto expectedBefore = capabilities.temporalFramesBefore - request.missingFramesBefore;
    const auto expectedAfter = capabilities.temporalFramesAfter - request.missingFramesAfter;
    if (expectedBefore > std::numeric_limits<std::size_t>::max() - expectedAfter - 1 ||
        request.context.size() != expectedBefore + expectedAfter + 1 || request.centerIndex != expectedBefore)
        throw std::invalid_argument("Video restoration frame window does not match its declared temporal context");
    if (request.context.empty()) throw std::invalid_argument("Video restoration request has no frames");

    const auto& center = request.context[request.centerIndex];
    validateFrame(center,"center");
    if (media::rifeSdrInputProfile(center.color) != request.inputProfile)
        throw std::invalid_argument("Video restoration input profile does not match the frame's validated SDR color metadata");
    for (std::size_t index = 0; index < request.context.size(); ++index) {
        const auto& frame = request.context[index];
        validateFrame(frame,"context");
        if (frame.width != center.width || frame.height != center.height || !sameColor(frame.color,center.color))
            throw std::invalid_argument("Video restoration context must use one resolution and color profile");
        if (index && !(*request.context[index - 1].timestamp < *frame.timestamp))
            throw std::invalid_argument("Video restoration context timestamps must be strictly increasing");
    }
    validateSceneBounds(request);
}

void validateResult(const Capabilities& capabilities, const Request& request, const Result& result) {
    validateRequest(capabilities,request);
    validateFrame(result.centerFrame,"output");
    if (!std::isfinite(result.elapsedSeconds) || result.elapsedSeconds < 0.0)
        throw std::invalid_argument("Video restoration elapsed time must be finite and non-negative");
    const auto& source = request.context[request.centerIndex];
    if (result.centerFrame.index != source.index || result.centerFrame.timestamp != source.timestamp || result.centerFrame.duration != source.duration)
        throw std::invalid_argument("Video restoration must preserve the center frame index, presentation time and duration");
    if (!sameColor(result.centerFrame.color,source.color))
        throw std::invalid_argument("Video restoration must preserve the center frame color metadata");
    const auto width = static_cast<std::int64_t>(source.width) * capabilities.scaleNumerator;
    const auto height = static_cast<std::int64_t>(source.height) * capabilities.scaleNumerator;
    if (width % capabilities.scaleDenominator != 0 || height % capabilities.scaleDenominator != 0 ||
        result.centerFrame.width != width / capabilities.scaleDenominator || result.centerFrame.height != height / capabilities.scaleDenominator)
        throw std::invalid_argument("Video restoration output dimensions do not match the declared model scale");
}

void Registry::registerBackend(std::shared_ptr<IVideoRestorationBackend> backend) {
    if (!backend) throw std::invalid_argument("Video restoration backend is null");
    const auto caps = backend->capabilities();
    validateCapabilities(caps);
    const Key key{caps.modelId,caps.adapterId,caps.adapterVersion};
    if (!backends_.emplace(key,std::move(backend)).second)
        throw std::invalid_argument("Video restoration backend identity is already registered");
}

std::shared_ptr<IVideoRestorationBackend> Registry::resolve(
    const std::string& modelId, const std::string& adapterId, int adapterVersion) const {
    const auto found = backends_.find(Key{modelId,adapterId,adapterVersion});
    if (found == backends_.end()) throw std::invalid_argument("No video restoration backend matches the verified model identity");
    return found->second;
}

} // namespace rifelove::restoration

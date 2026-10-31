#include "restoration/temporal_window.hpp"
#include "media/rife_color.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rifelove::restoration {

TemporalWindow::TemporalWindow(Capabilities capabilities) : capabilities_(std::move(capabilities)) {
    validateCapabilities(capabilities_);
    if (!capabilities_.supportsBoundaryPadding || capabilities_.tasks.size() != 1 ||
        std::find(capabilities_.placements.begin(),capabilities_.placements.end(),Placement::sourceRateBeforeInterpolation) == capabilities_.placements.end())
        throw std::invalid_argument("Temporal restoration scheduler needs a scene-boundary-capable source-rate model");
}

Request TemporalWindow::takeCenter() {
    Request request;
    request.task = capabilities_.tasks.front();
    request.placement = Placement::sourceRateBeforeInterpolation;
    const auto before = std::min(nextCenter_,capabilities_.temporalFramesBefore);
    const auto after = std::min(frames_.size() - nextCenter_ - 1,capabilities_.temporalFramesAfter);
    request.missingFramesBefore = capabilities_.temporalFramesBefore - before;
    request.missingFramesAfter = capabilities_.temporalFramesAfter - after;
    request.centerIndex = before;
    request.inputProfile = media::rifeSdrInputProfile(frames_[nextCenter_].color);
    if (!sceneFirstIndex_ || sceneSerial_ == 0)
        throw std::logic_error("Temporal restoration scene state is not initialized");
    request.sceneBounds = SceneBounds{*sceneFirstIndex_,sceneEndKnown_ ? sceneLastIndex_ : std::nullopt,sceneSerial_};
    for (auto index = nextCenter_ - before; index <= nextCenter_ + after; ++index)
        request.context.push_back(frames_[index]);
    validateRequest(capabilities_,request);
    ++nextCenter_;
    discardHistory();
    return request;
}

void TemporalWindow::discardHistory() {
    while (nextCenter_ > capabilities_.temporalFramesBefore) {
        frames_.pop_front();
        --nextCenter_;
    }
}

std::vector<Request> TemporalWindow::push(media::DecodedFrame frame, bool sceneCutBefore) {
    if (finished_) throw std::invalid_argument("Cannot append after finishing a temporal restoration window");
    if (!frame.timestamp || (lastTimestamp_ && *frame.timestamp <= *lastTimestamp_) ||
        (lastIndex_ && frame.index <= *lastIndex_))
        throw std::invalid_argument("Temporal restoration frames must have increasing indices and exact presentation timestamps");
    // Validate this frame before mutating the pending scene. Full temporal
    // uniformity is checked by validateRequest when a window is released.
    auto single = capabilities_;
    single.temporalFramesBefore = 0;
    single.temporalFramesAfter = 0;
    Request check;
    check.task = single.tasks.front();
    check.placement = Placement::sourceRateBeforeInterpolation;
    check.context = {frame};
    check.inputProfile = media::rifeSdrInputProfile(frame.color);
    validateRequest(single,check);
    if (!sceneCutBefore && !frames_.empty()) {
        single.temporalFramesBefore = 1;
        check.context = {frames_.back(),frame};
        check.centerIndex = 1;
        validateRequest(single,check);
    }
    std::vector<Request> ready;
    if (sceneCutBefore) {
        if (!frames_.empty()) {
            sceneLastIndex_ = frames_.back().index;
            sceneEndKnown_ = true;
        }
        while (nextCenter_ < frames_.size()) ready.push_back(takeCenter());
        frames_.clear();
        nextCenter_ = 0;
        sceneFirstIndex_.reset();
        sceneLastIndex_.reset();
        sceneEndKnown_ = false;
    }
    lastTimestamp_ = frame.timestamp;
    lastIndex_ = frame.index;
    if (!sceneFirstIndex_) {
        sceneFirstIndex_ = frame.index;
        sceneLastIndex_.reset();
        sceneEndKnown_ = false;
        ++sceneSerial_;
    }
    frames_.push_back(std::move(frame));
    while (nextCenter_ + capabilities_.temporalFramesAfter < frames_.size()) ready.push_back(takeCenter());
    return ready;
}

std::vector<Request> TemporalWindow::finish() {
    if (finished_) return {};
    std::vector<Request> ready;
    if (!frames_.empty()) {
        sceneLastIndex_ = frames_.back().index;
        sceneEndKnown_ = true;
    }
    while (nextCenter_ < frames_.size()) ready.push_back(takeCenter());
    frames_.clear();
    nextCenter_ = 0;
    finished_ = true;
    return ready;
}

} // namespace rifelove::restoration

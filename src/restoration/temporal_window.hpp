#pragma once

#include "restoration/video_restoration_backend.hpp"

#include <deque>

namespace rifelove::restoration {

// Bounded source-rate scheduler. It never invents timestamps, crosses a scene
// cut or hides clip-edge context. The adapter explicitly pads missing context.
class TemporalWindow final {
public:
    explicit TemporalWindow(Capabilities capabilities);
    [[nodiscard]] std::vector<Request> push(media::DecodedFrame frame, bool sceneCutBefore = false);
    [[nodiscard]] std::vector<Request> finish();
    [[nodiscard]] std::size_t bufferedFrames() const noexcept { return frames_.size(); }
private:
    [[nodiscard]] Request takeCenter();
    void discardHistory();
    Capabilities capabilities_;
    std::deque<media::DecodedFrame> frames_;
    std::size_t nextCenter_ = 0;
    std::optional<Rational> lastTimestamp_;
    std::optional<std::size_t> lastIndex_;
    bool finished_ = false;
};

} // namespace rifelove::restoration

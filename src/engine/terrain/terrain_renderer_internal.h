#pragma once
#include "render_device.h"
#include <vector>
#include <cstdint>
#include <limits>

namespace terrain_internal {

// Keep an in-flight overlay-max readback buffer alive on the device's
// fence-driven delete queue until bgfx retires the frame it was submitted in,
// then free it (drained at shutdown). Replaces the old file-static orphan deque,
// which leaked on shutdown and could mis-collect across frame-counter wraparound.
inline void stashOverlayReadback(std::vector<float>&& data, uint32_t submitFrame)
{
    if (data.empty())
        return;
    RenderDevice::instance().deferUntilFrameRetired(
        [kept = std::move(data)]() {}, submitFrame);
}

inline uint32_t currentFrameId()
{
    const uint32_t frameId = RenderDevice::instance().lastFrameId();
    return frameId == 0 ? std::numeric_limits<uint32_t>::max() : frameId;
}

} // namespace terrain_internal

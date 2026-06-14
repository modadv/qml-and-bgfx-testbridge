#pragma once

// Minimalist-2026 render resource-lifetime helpers.
//
// DeferredDeleteQueue runs a cleanup closure once the GPU has retired the frame
// the closure was scheduled against. It replaces the two patterns this codebase
// grew organically:
//   * hand-rolled "wait N frames, then free/destroy" counters
//     (e.g. m_textureSwapDelay / m_deferSmapUseFrames), and
//   * file-static "orphaned readback" deques that leaked on shutdown and could
//     mis-collect across the uint32 frame-counter wraparound.
//
// Ownership model: RenderDevice owns one queue, drives collect() from
// endFrame() with bgfx's retired frame id, and flushAll() at shutdown (so any
// closure that calls bgfx::destroy runs before bgfx::shutdown()). Callers
// schedule work with RenderDevice::deferUntilFrameRetired().
//
// C++14, header-only, thread-safe.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace engine
{

class DeferredDeleteQueue
{
public:
    // Sentinel "frame never retires": items tagged with it are only run by
    // flushAll() (at shutdown), never by collect(). Matches the codebase's use
    // of numeric_limits<uint32_t>::max() as an "unknown frame" marker, so a
    // readback whose submit frame was unknown is held rather than freed early.
    static constexpr uint32_t kNeverRetired = 0xFFFFFFFFu;

    // Schedule fn() to run once collect() is called with a retired id at or
    // after safeAfterFrame. The closure typically owns (by move-capture) the
    // resource being kept alive until then.
    void enqueue(std::function<void()> fn, uint32_t safeAfterFrame)
    {
        if (!fn)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(Item{ std::move(fn), safeAfterFrame });
    }

    // Run every closure whose safe frame has been retired by retiredFrame.
    // Closures run outside the lock since they may re-enter (bgfx::destroy,
    // and via destruction they may free buffers / schedule nothing new here).
    void collect(uint32_t retiredFrame)
    {
        std::deque<std::function<void()>> ready;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_pending.begin(); it != m_pending.end();)
            {
                if (isRetired(retiredFrame, it->safeAfterFrame))
                {
                    ready.push_back(std::move(it->fn));
                    it = m_pending.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
        for (auto& fn : ready)
            fn();
        // `ready` clears here: closures (and their move-captured resources) are
        // destroyed after running, which is what frees the kept-alive buffer.
    }

    // Run all pending closures immediately, regardless of frame. Call at
    // shutdown so nothing is leaked and any bgfx::destroy happens before
    // bgfx::shutdown().
    void flushAll()
    {
        std::deque<Item> pending;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pending.swap(m_pending);
        }
        for (auto& item : pending)
            item.fn();
    }

    size_t pendingCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending.size();
    }

private:
    struct Item
    {
        std::function<void()> fn;
        uint32_t safeAfterFrame;
    };

    // Wraparound-safe "has retiredFrame reached safeFrame?". Signed difference
    // stays correct as the uint32 frame counter wraps, provided the two values
    // are within 2^31 frames of each other (always true in practice).
    static bool isRetired(uint32_t retiredFrame, uint32_t safeFrame)
    {
        if (safeFrame == kNeverRetired)
            return false;
        return int32_t(retiredFrame - safeFrame) >= 0;
    }

    mutable std::mutex m_mutex;
    std::deque<Item> m_pending;
};

} // namespace engine

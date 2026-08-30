// readback_presenter.h
#pragma once

#include "render_content.h"
#include "terrain/render_device.h"

#include <QByteArray>
#include <QSize>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>

class QOpenGLFramebufferObject;

// One blit issued from the bgfx color target into a readback texture, awaiting
// the >=1 frame latency before bgfx::readTexture can pull it to the CPU.
struct BlitRecord
{
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint64_t frameIndex = 0;
    int width = 0;
    int height = 0;
    uint8_t texIndex = 0;
};

// A CPU-side readback issued via bgfx::readTexture, ready to upload into the Qt
// FBO texture once bgfx retires frameId.
struct PendingRead
{
    uint32_t frameId = std::numeric_limits<uint32_t>::max();
    int width = 0;
    int height = 0;
    QByteArray pixels;
    uint8_t texIndex = 0;
};

// Owns the offscreen bgfx surface and the GPU->CPU readback ring, and uploads
// completed readbacks into the Qt FBO texture via glTexSubImage2D. This is the
// reusable "present" path: any IRenderContent can be drawn through it without the
// QFBO renderer knowing how surface lifecycle or async readback work.
class ReadbackPresenter
{
public:
    ReadbackPresenter() = default;
    ~ReadbackPresenter() = default;

    ReadbackPresenter(const ReadbackPresenter&) = delete;
    ReadbackPresenter& operator=(const ReadbackPresenter&) = delete;

    // Acquire the device and (re)create the offscreen surface for sz, binding it as
    // content's render target and resizing content when needed. Clears the freshly
    // bound Qt FBO once on (re)create/resize so transient frames show a clean
    // background instead of uninitialized video memory. Returns false if the
    // device/surface is not ready this frame (caller should skip rendering).
    bool ensureSurface(const QSize& sz, void* nativeWindowHandle, IRenderContent* content);

    // Upload every readback whose frame has retired into the Qt FBO color texture.
    void processCompletedReadbacks(QOpenGLFramebufferObject* fbo);

    // Issue bgfx::readTexture for blits recorded at least one frame ago.
    void scheduleReadbacksFromQueue();

    // If a readback slot is free, blit the surface color target into it and record
    // it for a later readTexture. No-op when all slots are in flight.
    void blitForReadback(const QSize& sz);

    // Advance bgfx by one frame; remembers the retired frame id and returns it.
    uint32_t endFrame();

    // True while any blit/readback slot is still occupied.
    bool hasInFlightReadbacks() const;

    // True while at least one readback slot is free, i.e. a new frame can be
    // blitted right now without waiting for earlier readbacks to land. Gating on
    // this instead of hasInFlightReadbacks() lets the ring actually pipeline:
    // content is produced every frame and each readback retires a few frames
    // later, instead of one readback at a time serialising the whole loop.
    bool hasFreeReadbackSlot() const;

    uint64_t frameIndex() const { return m_frameIndex; }
    void bumpFrameIndex() { ++m_frameIndex; }
    uint32_t lastFrameId() const { return m_lastFrameId; }

    const RenderDevice::ViewSurface& surface() const { return m_surface; }

    // Drain in-flight readbacks (handing buffers to the device's fence queue),
    // destroy the surface, and release the device. Safe to call at shutdown.
    void drainAndRelease();

private:
    void waitForPendingReadbacks();
    void resetReadbackState();

    RenderDevice::ViewSurface m_surface;

    bool  m_runtimeInited = false;
    bool  m_sceneInited   = false;
    QSize m_lastSize;

    uint64_t m_frameIndex  = 0;
    uint32_t m_lastFrameId = std::numeric_limits<uint32_t>::max();

    std::deque<BlitRecord>  m_readyForRead;
    std::deque<PendingRead> m_pendingReads;
    std::array<bool, RenderDevice::kReadbackBufferCount> m_readbackInUse{};
    uint8_t m_nextReadbackIndex = 0;
};

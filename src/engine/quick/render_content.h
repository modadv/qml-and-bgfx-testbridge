// render_content.h
#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>

// Minimal contract the ReadbackPresenter needs to drive arbitrary render content
// through the offscreen surface + GPU->CPU readback present path. RenderScene
// implements it, so the present path is no longer welded to a concrete
// TerrainRenderer and can be reused for other content providers.
//
// Kept deliberately thin: only the per-frame present operations live here. Scene-
// specific commands (load heightfield, overlays, picking, live-shader reload) are
// not part of this interface — they stay on the concrete content type.
class IRenderContent
{
public:
    virtual ~IRenderContent() = default;

    // Resize the content's internal targets to match the surface.
    virtual void resize(uint32_t w, uint32_t h) = 0;

    // Advance simulation/animation by dt seconds and submit draw calls.
    virtual void update(float dt) = 0;

    // Bind the offscreen view + framebuffer the content should render into.
    virtual void setRenderTarget(bgfx::ViewId viewId, bgfx::FrameBufferHandle framebuffer) = 0;

    // Whether the content still needs to drive frames on its own (animation /
    // settling). False lets the viewport render on-demand instead of spinning.
    virtual bool needsContinuousUpdate() const = 0;
};

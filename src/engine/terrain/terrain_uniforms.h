#pragma once

#include <bgfx/bgfx.h>
#include "terrain_patch_tables.h"

class Uniforms {
public:
    void init();
    void submit();
    void destroy();
    void invalidate();
    
    bool isValid() const;

    // Uniform parameters
    union {
        struct {
            float dmapFactor;
            float lodFactor; 
            float cull;
            float freeze;

            float gpuSubd;
            float dmapBias;
            float padding1;
            float padding2;

            float terrainHalfWidth;
            float terrainHalfHeight;
        };
        float params[tables::kNumVec4 * 4];
    };

private:
    bgfx::UniformHandle m_paramsHandle = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_aspectParamsHandle = BGFX_INVALID_HANDLE;
};

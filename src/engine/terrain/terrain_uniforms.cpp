#include "terrain_uniforms.h"

void Uniforms::init() {
    m_paramsHandle = bgfx::createUniform("u_params", bgfx::UniformType::Vec4, tables::kNumVec4);
    m_aspectParamsHandle = bgfx::createUniform("u_aspectParams", bgfx::UniformType::Vec4);

    cull = 1.0f;
    freeze = 0.0f;
    gpuSubd = 3.0f;
    dmapBias = 0.0f;
    terrainHalfWidth = 1.0f;
    terrainHalfHeight = 1.0f;
}

void Uniforms::submit() {
    if (!isValid()) {
        return;
    }
    
    bgfx::setUniform(m_paramsHandle, params, tables::kNumVec4);
    
    float aspectParams[4] = { terrainHalfWidth, terrainHalfHeight, 0.0f, 0.0f };
    bgfx::setUniform(m_aspectParamsHandle, aspectParams);
}

void Uniforms::destroy() {
    if (bgfx::isValid(m_paramsHandle)) {
        bgfx::destroy(m_paramsHandle);
    }
    if (bgfx::isValid(m_aspectParamsHandle)) {
        bgfx::destroy(m_aspectParamsHandle);
    }

    invalidate();
}

void Uniforms::invalidate() {
    m_paramsHandle = BGFX_INVALID_HANDLE;
    m_aspectParamsHandle = BGFX_INVALID_HANDLE;
}

bool Uniforms::isValid() const {
    return bgfx::isValid(m_paramsHandle) && bgfx::isValid(m_aspectParamsHandle);
}

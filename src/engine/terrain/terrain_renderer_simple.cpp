// terrain_renderer_simple.cpp
//
// NoCompute fallback rendering path for TerrainRenderer: a fixed-resolution grid
// mesh sampled against the heightfield, with a CPU-generated slope map (smap).
// Used on GL 3.3 / GLES 3.0 / Mesa llvmpipe targets without compute shaders
// (the Ubuntu-18.04 software tier). Split out of the TerrainRenderer god class so
// the software path is isolated from the compute/isubd path; it implements the
// same TerrainRenderer methods declared in terrain_renderer.h.

#include "terrain_renderer.h"
#include "terrain_types.h"
#include "terrain_cpu_compute.h"
#include "logger.h"

#include <bgfx/bgfx.h>
#include <cstring>
#include <vector>

namespace {
constexpr int kSimpleGridDim = 256; // 256x256 quad mesh, ~131k triangles
}

void TerrainRenderer::loadSimpleGridBuffers()
{
    if (bgfx::isValid(m_simpleGridVertices) && bgfx::isValid(m_simpleGridIndices))
    {
        return;
    }

    m_simpleGridLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .end();

    const int dim = kSimpleGridDim;
    const int vertCount = (dim + 1) * (dim + 1);
    const int quadCount = dim * dim;
    const int indexCount = quadCount * 6;

    const bgfx::Memory* vmem = bgfx::alloc(uint32_t(vertCount) * sizeof(float) * 3);
    float* vdata = reinterpret_cast<float*>(vmem->data);
    const float invDim = 1.0f / float(dim);
    for (int j = 0; j <= dim; ++j)
    {
        for (int i = 0; i <= dim; ++i)
        {
            const float u = float(i) * invDim;
            const float v = float(j) * invDim;
            float* p = vdata + (size_t(j) * size_t(dim + 1) + size_t(i)) * 3;
            p[0] = u * 2.0f - 1.0f;
            p[1] = v * 2.0f - 1.0f;
            p[2] = 0.0f;
        }
    }

    const bool use32 = vertCount > 65535;
    const uint32_t indexStride = use32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const bgfx::Memory* imem = bgfx::alloc(uint32_t(indexCount) * indexStride);

    if (use32)
    {
        uint32_t* idata = reinterpret_cast<uint32_t*>(imem->data);
        uint32_t* w = idata;
        for (int j = 0; j < dim; ++j)
        {
            for (int i = 0; i < dim; ++i)
            {
                const uint32_t a = uint32_t(j * (dim + 1) + i);
                const uint32_t b = a + 1;
                const uint32_t c = uint32_t((j + 1) * (dim + 1) + i);
                const uint32_t d = c + 1;
                *w++ = a; *w++ = c; *w++ = b;
                *w++ = b; *w++ = c; *w++ = d;
            }
        }
    }
    else
    {
        uint16_t* idata = reinterpret_cast<uint16_t*>(imem->data);
        uint16_t* w = idata;
        for (int j = 0; j < dim; ++j)
        {
            for (int i = 0; i < dim; ++i)
            {
                const uint16_t a = uint16_t(j * (dim + 1) + i);
                const uint16_t b = uint16_t(a + 1);
                const uint16_t c = uint16_t((j + 1) * (dim + 1) + i);
                const uint16_t d = uint16_t(c + 1);
                *w++ = a; *w++ = c; *w++ = b;
                *w++ = b; *w++ = c; *w++ = d;
            }
        }
    }

    m_simpleGridVertices = bgfx::createVertexBuffer(vmem, m_simpleGridLayout);
    m_simpleGridIndices  = bgfx::createIndexBuffer(imem, use32 ? BGFX_BUFFER_INDEX32 : BGFX_BUFFER_NONE);
    m_simpleGridIndexCount = uint32_t(indexCount);
}

void TerrainRenderer::renderTerrainSimple()
{
    if (!bgfx::isValid(m_simpleGridVertices) || !bgfx::isValid(m_simpleGridIndices))
    {
        return;
    }

    const uint8_t viewId = m_viewId;
    bgfx::touch(viewId);

    float model[16];
    buildModelMatrix(model);

    bgfx::setTransform(model);

    bgfx::setTexture(0,
                     m_samplers[types::TERRAIN_DMAP_SAMPLER],
                     m_textures[types::TEXTURE_DMAP],
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                         | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);

    bgfx::setTexture(1,
                     m_samplers[types::TERRAIN_SMAP_SAMPLER],
                     m_textures[types::TEXTURE_SMAP],
                     BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                         | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

    if (bgfx::isValid(m_textures[types::TEXTURE_DIFFUSE]))
    {
        const uint32_t diffuseFlags = BGFX_SAMPLER_UVW_MIRROR
            | BGFX_SAMPLER_MIN_ANISOTROPIC
            | BGFX_SAMPLER_MAG_ANISOTROPIC
            | BGFX_SAMPLER_MIP_POINT;
        bgfx::setTexture(5,
                         m_samplers[types::TERRAIN_DIFFUSE_SAMPLER],
                         m_textures[types::TEXTURE_DIFFUSE],
                         diffuseFlags);
    }
    else
    {
        // Always bind something to slot 5 so the fragment shader's u_DiffuseSampler
        // is not left unbound (sampling an unbound texture is undefined behavior on
        // some GL drivers and triggers GL_INVALID_OPERATION at the next GL call).
        bgfx::TextureHandle fallback = bgfx::isValid(m_textures[types::TEXTURE_DMAP])
            ? m_textures[types::TEXTURE_DMAP]
            : m_dummySmap;
        if (bgfx::isValid(fallback))
        {
            bgfx::setTexture(5,
                             m_samplers[types::TERRAIN_DIFFUSE_SAMPLER],
                             fallback,
                             BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        }
    }

    if (bgfx::isValid(m_diffuseUvParamsHandle))
    {
        const float uvParams[4] = { float(m_diffuseUvMode), 0.0f, 0.0f, 0.0f };
        bgfx::setUniform(m_diffuseUvParamsHandle, uvParams);
    }

    bgfx::setVertexBuffer(0, m_simpleGridVertices);
    bgfx::setIndexBuffer(m_simpleGridIndices, 0, m_simpleGridIndexCount);

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;
    if (m_wireframe) state |= BGFX_STATE_PT_LINES;
    bgfx::setState(state);

    m_uniforms.submit();

    const int shadingIdx = (m_shading >= 0 && m_shading < types::SHADING_COUNT) ? m_shading : 0;
    bgfx::ProgramHandle prog = m_programsSimpleDraw[shadingIdx];
    if (!bgfx::isValid(prog))
    {
        prog = m_programsSimpleDraw[types::PROGRAM_TERRAIN];
    }
    if (!bgfx::isValid(prog))
    {
        return;
    }
    bgfx::submit(viewId, prog);
}

void TerrainRenderer::cpuRegenerateSmap()
{
    if (m_dmapNormalizedCpu.empty()
        || m_heightfieldWidth == 0 || m_heightfieldHeight == 0)
    {
        return;
    }

    std::vector<float> rgba;
    engine::cpuGenerateSmap(m_dmapNormalizedCpu.data(),
                            int(m_heightfieldWidth), int(m_heightfieldHeight),
                            rgba);

    // Create empty RGBA32F then updateTexture2D — same Mesa/Intel workaround
    // as the NoCompute dmap upload: initial data via glTexImage2D with a
    // floating-point internal format can leave GL in an error state.
    // Pack slope (sx,sy) into RG16F to avoid RGBA32F issues on Mesa/Intel
    // legacy GL path. Shader only samples .rg so remaining channels are
    // irrelevant. Use empty-create + updateTexture2D to dodge alignment
    // issues that bite initial-data uploads on some drivers.
    std::vector<uint16_t> rg16(size_t(m_heightfieldWidth) * size_t(m_heightfieldHeight) * 2u, 0);
    auto floatToHalf = [](float f) -> uint16_t {
        uint32_t x;
        std::memcpy(&x, &f, sizeof(x));
        const uint32_t sign = (x >> 16) & 0x8000u;
        int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
        uint32_t mant = (x >> 13) & 0x3ff;
        if (exp <= 0) { return uint16_t(sign); }
        if (exp >= 31) { return uint16_t(sign | 0x7c00u); }
        return uint16_t(sign | (uint32_t(exp) << 10) | mant);
    };
    const size_t count = size_t(m_heightfieldWidth) * size_t(m_heightfieldHeight);
    for (size_t i = 0; i < count; ++i)
    {
        rg16[i * 2 + 0] = floatToHalf(rgba[i * 4 + 0]);
        rg16[i * 2 + 1] = floatToHalf(rgba[i * 4 + 1]);
    }

    bgfx::TextureHandle newSmap = bgfx::createTexture2D(
        m_heightfieldWidth, m_heightfieldHeight, false, 1,
        bgfx::TextureFormat::RG16F,
        BGFX_TEXTURE_NONE);
    LOG_I("[TerrainRenderer] cpuRegenerateSmap RG16F {}x{} flags=NONE handle={} valid={}",
          m_heightfieldWidth, m_heightfieldHeight, newSmap.idx, bgfx::isValid(newSmap));
    if (bgfx::isValid(newSmap))
    {
        const bgfx::Memory* mem = bgfx::copy(rg16.data(),
                                             uint32_t(rg16.size() * sizeof(uint16_t)));
        bgfx::updateTexture2D(newSmap, 0, 0, 0, 0,
                              m_heightfieldWidth, m_heightfieldHeight, mem);
    }

    deferDestroyTexture(m_textures[types::TEXTURE_SMAP], kTextureRetireFrames);
    m_textures[types::TEXTURE_SMAP] = newSmap;
    m_smapNeedsRegen = false;
    m_cpuSmapGenTime = 0.0f;
}

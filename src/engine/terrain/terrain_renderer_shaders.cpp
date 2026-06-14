// terrain_renderer_shaders.cpp
//
// Shader program loading and the live-shader hot-reload state machine for
// TerrainRenderer. Split out of the TerrainRenderer god class; implements
// methods declared in terrain_renderer.h.

#include "terrain_renderer.h"
#include "terrain_types.h"
#include "logger.h"
#include "common/bgfx_utils.h"
#include "render_device.h"
#include "render_capabilities.h"
#include <bgfx/bgfx.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <utility>

namespace {

bgfx::ShaderHandle loadShaderBinaryFile(const std::string& path,
                                         std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "failed to open compiled shader: " + path;
        return BGFX_INVALID_HANDLE;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        error = "compiled shader is empty: " + path;
        return BGFX_INVALID_HANDLE;
    }
    file.seekg(0, std::ios::beg);

    std::vector<char> bytes(static_cast<size_t>(size));
    if (!file.read(bytes.data(), size)) {
        error = "failed to read compiled shader: " + path;
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* mem = bgfx::copy(bytes.data(), uint32_t(bytes.size()));
    bgfx::ShaderHandle shader = bgfx::createShader(mem);
    if (!bgfx::isValid(shader)) {
        error = "bgfx::createShader failed for: " + path;
    }
    return shader;
}

}

void TerrainRenderer::requestLiveShader(const std::string& slot,
                                        const std::string& binPath,
                                        const std::string& hash)
{
    LiveShaderSlotState* state = nullptr;
    if (slot == "terrain_simple.vertex")
        state = &m_liveTerrainSimpleVertex;
    else if (slot == "terrain_simple.fragment")
        state = &m_liveTerrainSimpleFragment;
    else if (slot == "overlay_max_elevation.compute")
        state = &m_liveOverlayMaxElevationCompute;

    if (!state)
    {
        LOG_E("[live-shader] unsupported slot {}", slot);
        return;
    }

    state->pendingBinPath = binPath;
    state->pendingHash = hash;
    state->pending = true;
    state->revertPending = false;
}

void TerrainRenderer::requestRevertLiveShader(const std::string& slot)
{
    LiveShaderSlotState* state = nullptr;
    if (slot == "terrain_simple.vertex")
        state = &m_liveTerrainSimpleVertex;
    else if (slot == "terrain_simple.fragment")
        state = &m_liveTerrainSimpleFragment;
    else if (slot == "overlay_max_elevation.compute")
        state = &m_liveOverlayMaxElevationCompute;

    if (!state)
    {
        LOG_E("[live-shader] unsupported revert slot {}", slot);
        return;
    }

    state->pending = false;
    state->revertPending = true;
}

bool TerrainRenderer::rebuildLiveTerrainSimpleProgram()
{
    if (!bgfx::isValid(m_originalSimpleTerrainProgram))
    {
        m_originalSimpleTerrainProgram = m_programsSimpleDraw[types::PROGRAM_TERRAIN];
    }

    if (!m_liveTerrainSimpleVertex.active && !m_liveTerrainSimpleFragment.active)
    {
        if (bgfx::isValid(m_programsSimpleDraw[types::PROGRAM_TERRAIN]) &&
            m_programsSimpleDraw[types::PROGRAM_TERRAIN].idx != m_originalSimpleTerrainProgram.idx)
        {
            bgfx::destroy(m_programsSimpleDraw[types::PROGRAM_TERRAIN]);
        }
        m_programsSimpleDraw[types::PROGRAM_TERRAIN] = m_originalSimpleTerrainProgram;
        m_originalSimpleTerrainProgram = BGFX_INVALID_HANDLE;
        return true;
    }

    std::string error;
    bgfx::ShaderHandle vsh = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fsh = BGFX_INVALID_HANDLE;

    if (m_liveTerrainSimpleVertex.active)
    {
        vsh = loadShaderBinaryFile(m_liveTerrainSimpleVertex.activeBinPath, error);
        if (!bgfx::isValid(vsh))
        {
            m_liveTerrainSimpleVertex.lastError = error;
            return false;
        }
    }
    else
    {
        vsh = loadShader("vs_terrain_simple");
        if (!bgfx::isValid(vsh))
        {
            m_liveTerrainSimpleVertex.lastError = "failed to load stock vertex shader vs_terrain_simple";
            return false;
        }
    }

    if (m_liveTerrainSimpleFragment.active)
    {
        fsh = loadShaderBinaryFile(m_liveTerrainSimpleFragment.activeBinPath, error);
        if (!bgfx::isValid(fsh))
        {
            bgfx::destroy(vsh);
            m_liveTerrainSimpleFragment.lastError = error;
            return false;
        }
    }
    else
    {
        fsh = loadShader("fs_terrain_simple");
        if (!bgfx::isValid(fsh))
        {
            bgfx::destroy(vsh);
            m_liveTerrainSimpleFragment.lastError = "failed to load stock fragment shader fs_terrain_simple";
            return false;
        }
    }

    bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, true);
    if (!bgfx::isValid(program))
    {
        m_liveTerrainSimpleVertex.lastError = "bgfx::createProgram failed for live terrain_simple program";
        m_liveTerrainSimpleFragment.lastError = "bgfx::createProgram failed for live terrain_simple program";
        return false;
    }

    if (bgfx::isValid(m_programsSimpleDraw[types::PROGRAM_TERRAIN]) &&
        (!bgfx::isValid(m_originalSimpleTerrainProgram) ||
         m_programsSimpleDraw[types::PROGRAM_TERRAIN].idx != m_originalSimpleTerrainProgram.idx))
    {
        bgfx::destroy(m_programsSimpleDraw[types::PROGRAM_TERRAIN]);
    }
    m_programsSimpleDraw[types::PROGRAM_TERRAIN] = program;
    return true;
}

void TerrainRenderer::applyPendingLiveShader()
{
    auto applyTerrainStage = [this](const char* slot, LiveShaderSlotState& state) {
        if (state.revertPending)
        {
            state.revertPending = false;
            state.active = false;
            state.activeHash.clear();
            state.activeBinPath.clear();
            state.lastError.clear();
            if (rebuildLiveTerrainSimpleProgram())
                LOG_I("[live-shader] reverted {}", slot);
            else
                LOG_E("[live-shader] failed to rebuild terrain program while reverting {}", slot);
        }

        if (!state.pending)
            return;

        state.pending = false;
        state.active = true;
        state.activeHash = state.pendingHash;
        state.activeBinPath = state.pendingBinPath;
        state.lastError.clear();
        if (rebuildLiveTerrainSimpleProgram())
        {
            LOG_I("[live-shader] applied {} hash={}", slot, state.activeHash);
            return;
        }

        state.active = false;
        LOG_E("[live-shader] {}", state.lastError);
        rebuildLiveTerrainSimpleProgram();
    };

    applyTerrainStage("terrain_simple.vertex", m_liveTerrainSimpleVertex);
    applyTerrainStage("terrain_simple.fragment", m_liveTerrainSimpleFragment);

    LiveShaderSlotState& compute = m_liveOverlayMaxElevationCompute;
    if (compute.revertPending)
    {
        compute.revertPending = false;
        if (compute.active)
        {
            if (bgfx::isValid(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]))
                bgfx::destroy(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]);
            m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = m_originalOverlayMaxElevationProgram;
            m_originalOverlayMaxElevationProgram = BGFX_INVALID_HANDLE;
            compute = LiveShaderSlotState{};
            LOG_I("[live-shader] reverted overlay_max_elevation.compute");
        }
    }

    if (compute.pending)
    {
        compute.pending = false;
        compute.lastError.clear();

        if (RenderDevice::renderCaps().noCompute())
        {
            compute.lastError = "compute shader live slot is unavailable in no-compute render tier";
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        std::string error;
        bgfx::ShaderHandle csh = loadShaderBinaryFile(compute.pendingBinPath, error);
        if (!bgfx::isValid(csh))
        {
            compute.lastError = error;
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        bgfx::ProgramHandle program = bgfx::createProgram(csh, true);
        if (!bgfx::isValid(program))
        {
            compute.lastError = "bgfx::createProgram failed for live overlay_max_elevation.compute";
            LOG_E("[live-shader] {}", compute.lastError);
            return;
        }

        if (!compute.active)
            m_originalOverlayMaxElevationProgram = m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION];
        else if (bgfx::isValid(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]))
            bgfx::destroy(m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION]);

        m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = program;
        compute.active = true;
        compute.activeHash = compute.pendingHash;
        compute.activeBinPath = compute.pendingBinPath;
        LOG_I("[live-shader] applied overlay_max_elevation.compute hash={}", compute.activeHash);
    }
}

nlohmann::json TerrainRenderer::liveShaderSnapshot() const
{
    auto slotJson = [](const char* name, const LiveShaderSlotState& state) {
        return nlohmann::json{
            {"slot", name},
            {"active", state.active},
            {"activeHash", state.activeHash},
            {"activeBinPath", state.activeBinPath},
            {"pending", state.pending},
            {"revertPending", state.revertPending},
            {"lastError", state.lastError}
        };
    };

    nlohmann::json slots = nlohmann::json::array({
        slotJson("terrain_simple.vertex", m_liveTerrainSimpleVertex),
        slotJson("terrain_simple.fragment", m_liveTerrainSimpleFragment),
        slotJson("overlay_max_elevation.compute", m_liveOverlayMaxElevationCompute)
    });

    nlohmann::json activeSlots = nlohmann::json::array();
    std::string firstActiveSlot;
    std::string firstActiveHash;
    std::string firstActiveBinPath;
    for (const auto& slot : slots)
    {
        if (slot.value("active", false))
        {
            activeSlots.push_back(slot["slot"]);
            if (firstActiveSlot.empty())
            {
                firstActiveSlot = slot.value("slot", std::string{});
                firstActiveHash = slot.value("activeHash", std::string{});
                firstActiveBinPath = slot.value("activeBinPath", std::string{});
            }
        }
    }

    const bool pending = m_liveTerrainSimpleVertex.pending ||
                         m_liveTerrainSimpleFragment.pending ||
                         m_liveOverlayMaxElevationCompute.pending;
    const bool revertPending = m_liveTerrainSimpleVertex.revertPending ||
                               m_liveTerrainSimpleFragment.revertPending ||
                               m_liveOverlayMaxElevationCompute.revertPending;

    return {
        {"enabled", true},
        {"supportedSlots", {"terrain_simple.vertex", "terrain_simple.fragment", "overlay_max_elevation.compute"}},
        {"active", !activeSlots.empty()},
        {"activeSlot", firstActiveSlot},
        {"activeHash", firstActiveHash},
        {"activeBinPath", firstActiveBinPath},
        {"activeSlots", activeSlots},
        {"slots", slots},
        {"pending", pending},
        {"revertPending", revertPending},
        {"lastError", ""}
    };
}

void TerrainRenderer::loadPrograms() {
    m_samplers[types::TERRAIN_DMAP_SAMPLER] = bgfx::createUniform("u_DmapSampler", bgfx::UniformType::Sampler);
    m_samplers[types::TERRAIN_SMAP_SAMPLER] = bgfx::createUniform("u_SmapSampler", bgfx::UniformType::Sampler);
    m_samplers[types::TERRAIN_DIFFUSE_SAMPLER] = bgfx::createUniform("u_DiffuseSampler", bgfx::UniformType::Sampler);
    m_samplers[types::HEIGHTFIELD_RAW_SAMPLER] = bgfx::createUniform("u_heightfieldRaw", bgfx::UniformType::Sampler);

    m_uniforms.init();

    m_smapParamsHandle = bgfx::createUniform("u_smapParams", bgfx::UniformType::Vec4);
    m_smapChunkParamsHandle = bgfx::createUniform("u_smapChunkParams", bgfx::UniformType::Vec4);
    m_heightfieldDecodeParamsHandle = bgfx::createUniform("u_heightfieldDecodeParams", bgfx::UniformType::Vec4);
    m_diffuseUvParamsHandle = bgfx::createUniform("u_diffuseUvParams", bgfx::UniformType::Vec4);

    m_rectMaxSampler = bgfx::createUniform("u_rectMaxSampler", bgfx::UniformType::Sampler);
    m_rectMaxParamsHandle = bgfx::createUniform("u_rectMaxParams", bgfx::UniformType::Vec4);
    m_rectViewParamsHandle = bgfx::createUniform("u_rectViewParams", bgfx::UniformType::Vec4);
    m_rectParamsHandle = bgfx::createUniform("u_rectParams", bgfx::UniformType::Vec4);
    m_rectSampleParamsHandle = bgfx::createUniform("u_rectSampleParams", bgfx::UniformType::Vec4);
    m_rectDebugParamsHandle = bgfx::createUniform("u_rectDebugParams", bgfx::UniformType::Vec4);

    const bool noCompute = RenderDevice::renderCaps().noCompute();
    if (noCompute)
    {
        m_programsSimpleDraw[types::PROGRAM_TERRAIN] = loadProgram("vs_terrain_simple", "fs_terrain_simple");
        m_programsSimpleDraw[types::PROGRAM_TERRAIN_NORMAL] = loadProgram("vs_terrain_simple", "fs_terrain_simple_normal");
        for (uint32_t i = 0; i < types::PROGRAM_COUNT; ++i)
        {
            m_programsCompute[i] = BGFX_INVALID_HANDLE;
        }
        m_programsDraw[types::PROGRAM_TERRAIN] = BGFX_INVALID_HANDLE;
        m_programsDraw[types::PROGRAM_TERRAIN_NORMAL] = BGFX_INVALID_HANDLE;
    }
    else
    {
        m_programsDraw[types::PROGRAM_TERRAIN] = loadProgram("vs_terrain_render", "fs_terrain_render");
        m_programsDraw[types::PROGRAM_TERRAIN_NORMAL] = loadProgram("vs_terrain_render", "fs_terrain_render_normal");

        m_programsCompute[types::PROGRAM_SUBD_CS_LOD] = bgfx::createProgram(loadShader("cs_terrain_lod"), true);
        m_programsCompute[types::PROGRAM_UPDATE_INDIRECT] = bgfx::createProgram(loadShader("cs_terrain_update_indirect"), true);
        m_programsCompute[types::PROGRAM_UPDATE_DRAW] = bgfx::createProgram(loadShader("cs_terrain_update_draw"), true);
        m_programsCompute[types::PROGRAM_INIT_INDIRECT] = bgfx::createProgram(loadShader("cs_terrain_init"), true);
        m_programsCompute[types::PROGRAM_GENERATE_SMAP] = bgfx::createProgram(loadShader("cs_generate_smap"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_MINMAX] = bgfx::createProgram(loadShader("cs_heightfield_minmax"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_REDUCE] = bgfx::createProgram(loadShader("cs_heightfield_reduce"), true);
        m_programsCompute[types::PROGRAM_HEIGHTFIELD_NORMALIZE] = bgfx::createProgram(loadShader("cs_heightfield_normalize"), true);
        m_programsCompute[types::PROGRAM_OVERLAY_MAX_ELEVATION] = bgfx::createProgram(loadShader("cs_overlay_max_elevation"), true);
    }

    if (noCompute)
    {
        m_useGpuSmap = false;
        m_useGpuHeightfieldDecode = false;
    }

    m_programRectWire = loadProgram("vs_rect_wire", "fs_rect_wire");
    m_programColor = loadProgram("vs_color", "fs_color");

    m_colorLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true, true)
        .end();
    m_colorLayoutReady = true;
}

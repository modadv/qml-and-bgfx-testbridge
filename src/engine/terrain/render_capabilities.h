#pragma once

#include <string>

namespace engine
{

enum class RenderTier
{
    Full = 0,      // GPU compute + indirect draw available (normal hardware)
    NoCompute = 1, // Software / limited backend (e.g. Mesa llvmpipe on Ubuntu 18.04)
};

enum class RenderTierOverride
{
    Auto = 0,
    Full,
    NoCompute,
};

struct RenderCaps
{
    RenderTier tier = RenderTier::Full;
    bool hasCompute      = true;
    bool hasIndirect     = true;
    bool hasImageRW      = true;
    bool isSoftwareBackend = false; // matched llvmpipe/softpipe/swrast in renderer string
    std::string rendererStr;        // glGetString(GL_RENDERER) when available
    std::string glVersionStr;       // glGetString(GL_VERSION) when available
    std::string overrideSource;     // "auto" | "env:full" | "env:nocompute"

    bool noCompute() const { return tier == RenderTier::NoCompute; }
};

// Reads TESTBRIDGE_RENDER_TIER env var: "full" | "nocompute" | "auto" (case-insensitive).
RenderTierOverride parseTierOverrideEnv();

} // namespace engine

#include "render_capabilities.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace engine
{

RenderTierOverride parseTierOverrideEnv()
{
    const char* raw = std::getenv("TESTBRIDGE_RENDER_TIER");
    if (raw == nullptr || raw[0] == '\0')
        return RenderTierOverride::Auto;

    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });

    if (v == "full")
        return RenderTierOverride::Full;
    if (v == "nocompute" || v == "no-compute" || v == "no_compute")
        return RenderTierOverride::NoCompute;
    return RenderTierOverride::Auto;
}

} // namespace engine

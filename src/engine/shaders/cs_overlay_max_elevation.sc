// Rectangle max height reduction (grid sampling)
#include "bgfx_compute.sh"

BUFFER_RO(u_rects, vec4, 0);
SAMPLER2D(u_DmapSampler, 1);
IMAGE2D_WR(u_rectMaxOut, r32f, 2);

uniform vec4 u_rectParams;       // x: rectCount
uniform vec4 u_rectSampleParams; // x: terrainHalfWidth, y: terrainHalfHeight, z: dmapFactor, w: dmapBias

#define SAMPLE_GRID 16
SHARED float sMax[SAMPLE_GRID * SAMPLE_GRID];

NUM_THREADS(SAMPLE_GRID, SAMPLE_GRID, 1)
void main()
{
    uint rectIndex = gl_WorkGroupID.x;
    if (rectIndex >= uint(u_rectParams.x))
        return;

    uint base = rectIndex * 2u;
    vec4 rect0 = u_rects[base];
    vec4 rect1 = u_rects[base + 1u];
    vec2 uVec = rect0.zw;
    vec2 vVec = rect1.xy;
    if (dot(uVec, uVec) <= 1e-8 || dot(vVec, vVec) <= 1e-8)
    {
    if (gl_LocalInvocationIndex == 0)
    {
        imageStore(u_rectMaxOut, ivec2(rectIndex, 0), vec4(0.0, 0.0, 0.0, 0.0));
    }
    return;
    }

    vec2 p00 = rect0.xy;
    vec2 t = (vec2(gl_LocalInvocationID.xy) + 0.5) / float(SAMPLE_GRID);
    vec2 pos = p00 + t.x * uVec + t.y * vVec;

    vec2 uv;
    uv.x = (pos.x + u_rectSampleParams.x) / (2.0 * u_rectSampleParams.x);
    uv.y = (pos.y + u_rectSampleParams.y) / (2.0 * u_rectSampleParams.y);
    uv = clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));

    float h = texture2DLod(u_DmapSampler, uv, 0.0).r * u_rectSampleParams.z + u_rectSampleParams.w;

    sMax[gl_LocalInvocationIndex] = h;
    barrier();

    for (uint stride = (SAMPLE_GRID * SAMPLE_GRID) / 2u; stride > 0u; stride >>= 1u)
    {
        if (gl_LocalInvocationIndex < stride)
        {
            float other = sMax[gl_LocalInvocationIndex + stride];
            if (other > sMax[gl_LocalInvocationIndex])
                sMax[gl_LocalInvocationIndex] = other;
        }
        barrier();
    }

    if (gl_LocalInvocationIndex == 0)
    {
        imageStore(u_rectMaxOut, ivec2(rectIndex, 0), vec4(sMax[0], 0.0, 0.0, 0.0));
    }
}

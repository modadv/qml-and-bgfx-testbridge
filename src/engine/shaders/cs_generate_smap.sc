// Terrain slope-map generation from a normalized heightfield.
#include "bgfx_compute.sh"
#include "uniforms.sh"

SAMPLER2D(u_heightfield, 0);

IMAGE2D_WR(u_smap_output, rgba32f, 1);

uniform vec4 u_smapParams;         // x: width, y: height, z: terrain_half_width, w: terrain_half_height
uniform vec4 u_smapChunkParams;    // x: chunkStartX, y: chunkStartY, z: chunkWidth, w: chunkHeight

NUM_THREADS(32, 32, 1)
void main()
{
    ivec2 globalCoord = ivec2(gl_GlobalInvocationID.xy);

    globalCoord += ivec2(u_smapChunkParams.xy);

    vec2 texSize = u_smapParams.xy;

    if(any(greaterThanEqual(globalCoord, ivec2(texSize))))
        return;

    int i = globalCoord.x;
    int j = globalCoord.y;
    int w = int(texSize.x);
    int h = int(texSize.y);

    int i1 = max(0, i - 1);
    int i2 = min(w - 1, i + 1);
    int j1 = max(0, j - 1);
    int j2 = min(h - 1, j + 1);

    vec2 texCoord_l = (vec2(i1, j) + 0.5) / texSize;
    vec2 texCoord_r = (vec2(i2, j) + 0.5) / texSize;
    vec2 texCoord_b = (vec2(i, j1) + 0.5) / texSize;
    vec2 texCoord_t = (vec2(i, j2) + 0.5) / texSize;

    float z_l = texture2DLod(u_heightfield, texCoord_l, 0.0).r;
    float z_r = texture2DLod(u_heightfield, texCoord_r, 0.0).r;
    float z_b = texture2DLod(u_heightfield, texCoord_b, 0.0).r;
    float z_t = texture2DLod(u_heightfield, texCoord_t, 0.0).r;

    float slope_x = float(w) * 0.5 * (z_r - z_l);
    float slope_y = float(h) * 0.5 * (z_t - z_b);

    imageStore(u_smap_output, globalCoord, vec4(slope_x, slope_y, 0.0, 0.0));
}

$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>
#include "uniforms.sh"

SAMPLER2D(u_DmapSampler, 0);

void main()
{
    // a_position is a vec3 with x,y in [-1,1] and z = 0 from the fixed grid VB.
    vec4 finalVertex = vec4(a_position.x, a_position.y, 0.0, 1.0);
    finalVertex.x *= u_terrainHalfWidth;
    finalVertex.y *= u_terrainHalfHeight;

    vec2 uv;
    uv.x = (finalVertex.x + u_terrainHalfWidth)  / (2.0 * u_terrainHalfWidth);
    uv.y = (finalVertex.y + u_terrainHalfHeight) / (2.0 * u_terrainHalfHeight);

    finalVertex.z = texture2DLod(u_DmapSampler, uv, 0.0).x * u_DmapFactor + u_DmapBias;

    v_texcoord0 = uv;

    gl_Position = mul(u_modelViewProj, finalVertex);
}

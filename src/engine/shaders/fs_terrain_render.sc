$input v_texcoord0

#include "bgfx_compute.sh"
#include "matrices.sh"
#include "isubd.sh"
#include "uniforms.sh"
#include "terrain_common.sh"
uniform vec4 u_diffuseUvParams; // x: uv mode (0:none,1:swap,2:rotCW,3:rotCCW), yzw: reserved

void main()
{
    // Approximate the normal from the slope map.
    vec2 s = texture2D(u_SmapSampler, v_texcoord0).rg * u_DmapFactor;
    vec3 n = normalize(vec3(-s, 1.0));

    // Simple directional lighting proxy with a small ambient floor.
    float lightFactor = clamp(n.z, 0.1, 1.0);

    // Sample the diffuse texture using interpolated terrain UVs.
    vec2 diffuseUv = v_texcoord0;
    if (u_diffuseUvParams.x > 2.5) {
        diffuseUv = vec2(1.0 - v_texcoord0.y, v_texcoord0.x);
    } else if (u_diffuseUvParams.x > 1.5) {
        diffuseUv = vec2(v_texcoord0.y, 1.0 - v_texcoord0.x);
    } else if (u_diffuseUvParams.x > 0.5) {
        diffuseUv = vec2(v_texcoord0.y, v_texcoord0.x);
    }
    vec4 diffuseTexColor = texture2D(u_DiffuseSampler, diffuseUv);

    // Combine diffuse color and lighting.
    vec3 finalColor = diffuseTexColor.rgb * lightFactor;

    // Output the shaded terrain color.
    gl_FragColor = vec4(finalColor, 1.0);
}

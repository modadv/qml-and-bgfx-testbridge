$input v_texcoord0

#include <bgfx_shader.sh>
#include "uniforms.sh"

SAMPLER2D(u_SmapSampler, 1);
SAMPLER2D(u_DiffuseSampler, 5);

uniform vec4 u_diffuseUvParams;

void main()
{
    vec2 s = texture2D(u_SmapSampler, v_texcoord0).rg * u_DmapFactor;
    vec3 n = normalize(vec3(-s, 1.0));

    float lightFactor = clamp(n.z, 0.1, 1.0);

    vec2 diffuseUv = v_texcoord0;
    if (u_diffuseUvParams.x > 2.5) {
        diffuseUv = vec2(1.0 - v_texcoord0.y, v_texcoord0.x);
    } else if (u_diffuseUvParams.x > 1.5) {
        diffuseUv = vec2(v_texcoord0.y, 1.0 - v_texcoord0.x);
    } else if (u_diffuseUvParams.x > 0.5) {
        diffuseUv = vec2(v_texcoord0.y, v_texcoord0.x);
    }
    vec4 diffuseTexColor = texture2D(u_DiffuseSampler, diffuseUv);

    vec3 finalColor = diffuseTexColor.rgb * lightFactor;
    gl_FragColor = vec4(finalColor, 1.0);
}

$input v_texcoord0

#include <bgfx_shader.sh>
#include "uniforms.sh"

SAMPLER2D(u_SmapSampler, 1);

void main()
{
    vec2 s = texture2D(u_SmapSampler, v_texcoord0).rg * u_DmapFactor;
    vec3 n = normalize(vec3(-s, 1.0));
    gl_FragColor = vec4(n * 0.5 + 0.5, 1.0);
}

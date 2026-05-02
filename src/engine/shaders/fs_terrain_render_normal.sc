$input v_texcoord0

#include "bgfx_compute.sh"
#include "matrices.sh"
#include "isubd.sh"
#include "uniforms.sh"
#include "terrain_common.sh"

void main()
{
	vec2 s = texture2D(u_SmapSampler, v_texcoord0).rg * u_DmapFactor;
	vec3 n = normalize(vec3(-s, 1));
	gl_FragColor = vec4(abs(n), 1);
}

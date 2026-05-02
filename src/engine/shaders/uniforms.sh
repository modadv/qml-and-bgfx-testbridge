
uniform vec4 u_params[2];

uniform vec4 u_aspectParams;
#define u_terrainHalfWidth u_aspectParams.x
#define u_terrainHalfHeight u_aspectParams.y

#define u_DmapFactor u_params[0].x
#define u_LodFactor u_params[0].y
#define u_cull u_params[0].z
#define u_freeze u_params[0].w
#define u_gpu_subd  int(u_params[1].x)
#define u_DmapBias u_params[1].y


#define COMPUTE_THREAD_COUNT 32u
#define UPDATE_INDIRECT_VALUE_DIVIDE 32u

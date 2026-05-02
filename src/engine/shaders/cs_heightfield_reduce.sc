// Heightfield min/max reduction (from RGBA32F tiles)
#include "bgfx_compute.sh"

IMAGE2D_RO(u_heightfieldMinmaxIn, rgba32f, 0);
IMAGE2D_WR(u_heightfieldMinmaxOut, rgba32f, 1);

#define GROUP_SIZE 16
SHARED float sMin[GROUP_SIZE * GROUP_SIZE];
SHARED float sMax[GROUP_SIZE * GROUP_SIZE];

NUM_THREADS(GROUP_SIZE, GROUP_SIZE, 1)
void main()
{
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
    uint localIndex = gl_LocalInvocationIndex;
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 inSize = imageSize(u_heightfieldMinmaxIn);

    float minVal = 1.0e20;
    float maxVal = -1.0e20;
    if (coord.x < inSize.x && coord.y < inSize.y)
    {
        vec2 mm = imageLoad(u_heightfieldMinmaxIn, coord).xy;
        minVal = mm.x;
        maxVal = mm.y;
    }

    sMin[localIndex] = minVal;
    sMax[localIndex] = maxVal;
    barrier();

    for (uint stride = (GROUP_SIZE * GROUP_SIZE) / 2; stride > 0; stride >>= 1)
    {
        if (localIndex < stride)
        {
            sMin[localIndex] = min(sMin[localIndex], sMin[localIndex + stride]);
            sMax[localIndex] = max(sMax[localIndex], sMax[localIndex + stride]);
        }
        barrier();
    }

    if (localIndex == 0u)
    {
        ivec2 outCoord = ivec2(gl_WorkGroupID.xy);
        imageStore(u_heightfieldMinmaxOut, outCoord, vec4(sMin[0], sMax[0], 0.0, 0.0));
    }
}

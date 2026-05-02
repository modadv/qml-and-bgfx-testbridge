// Heightfield min/max reduction (tile-based)
#include "bgfx_compute.sh"

SAMPLER2D(u_heightfieldRaw, 0);
IMAGE2D_WR(u_heightfieldMinmax, rgba32f, 1);

uniform vec4 u_heightfieldDecodeParams; // x: width, y: height, z: decodeMode(0=int32,1=float32), w: orderId(0-3 BGRA, 4-7 RGBA)

#define GROUP_SIZE 16
SHARED float sMin[GROUP_SIZE * GROUP_SIZE];
SHARED float sMax[GROUP_SIZE * GROUP_SIZE];

float makeNaN()
{
    return uintBitsToFloat(0x7fc00000u);
}

bool isValidFloat(float v)
{
    return (v == v) && (abs(v) < 1.0e20);
}

float decodeHeight(vec4 texel)
{
    uvec4 bytes = uvec4(texel * 255.0 + 0.5);
    int orderPacked = int(u_heightfieldDecodeParams.w + 0.5);
    bool baseIsBGRA = orderPacked < 4;
    int orderId = orderPacked & 3;

    uint b0 = baseIsBGRA ? bytes.b : bytes.r;
    uint b1 = bytes.g;
    uint b2 = baseIsBGRA ? bytes.r : bytes.b;
    uint b3 = bytes.a;

    uint o0 = b0;
    uint o1 = b1;
    uint o2 = b2;
    uint o3 = b3;

    if (orderId == 1)
    {
        o0 = b2; o1 = b1; o2 = b0; o3 = b3;
    }
    else if (orderId == 2)
    {
        o0 = b3; o1 = b2; o2 = b1; o3 = b0;
    }
    else if (orderId == 3)
    {
        o0 = b3; o1 = b0; o2 = b1; o3 = b2;
    }

    uint packedValue = o0 | (o1 << 8) | (o2 << 16) | (o3 << 24);

    if (u_heightfieldDecodeParams.z < 0.5)
    {
        int v = int(packedValue);
        if (v == -1000000 || v == 1000000 || v == -500000)
            return makeNaN();
        return float(v) / 100.0;
    }

    float f = uintBitsToFloat(packedValue);
    if (f == -1000000.0 || f == 1000000.0 || f == -500000.0)
        return makeNaN();
    return f;
}

NUM_THREADS(GROUP_SIZE, GROUP_SIZE, 1)
void main()
{
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 base = ivec2(gl_WorkGroupID.xy) * GROUP_SIZE;
    ivec2 local = ivec2(gl_LocalInvocationID.xy);
    uint localIndex = gl_LocalInvocationIndex;

    float minVal = 1.0e20;
    float maxVal = -1.0e20;

    if (coord.x < int(u_heightfieldDecodeParams.x) && coord.y < int(u_heightfieldDecodeParams.y))
    {
        vec2 texSize = u_heightfieldDecodeParams.xy;
        vec2 texCoord = (vec2(coord) + 0.5) / texSize;
        vec4 texel = texture2DLod(u_heightfieldRaw, texCoord, 0.0);
        float h = decodeHeight(texel);
        if (isValidFloat(h))
        {
            minVal = h;
            maxVal = h;
        }
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
        imageStore(u_heightfieldMinmax, outCoord, vec4(sMin[0], sMax[0], 0.0, 0.0));
    }
}

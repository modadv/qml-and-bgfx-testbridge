// Heightfield normalization to DMap (R32F, quantized to 16-bit steps)
#include "bgfx_compute.sh"

SAMPLER2D(u_heightfieldRaw, 0);
IMAGE2D_RO(u_heightfieldMinmax, rgba32f, 1);
IMAGE2D_WR(u_heightfieldOut, r32f, 2);

uniform vec4 u_heightfieldDecodeParams; // x: width, y: height, z: decodeMode(0=int32,1=float32), w: orderId(0-3 BGRA, 4-7 RGBA)

#define GROUP_SIZE 16

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
    if (coord.x >= int(u_heightfieldDecodeParams.x) || coord.y >= int(u_heightfieldDecodeParams.y))
        return;

    vec2 minmax = imageLoad(u_heightfieldMinmax, ivec2(0, 0)).xy;
    if (!isValidFloat(minmax.x) || !isValidFloat(minmax.y) || minmax.y <= minmax.x)
    {
        imageStore(u_heightfieldOut, coord, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    vec2 texSize = u_heightfieldDecodeParams.xy;
    vec2 texCoord = (vec2(coord) + 0.5) / texSize;
    float h = decodeHeight(texture2DLod(u_heightfieldRaw, texCoord, 0.0));

    bool shift = minmax.x < 0.0;
    float range = shift ? (minmax.y - minmax.x) : minmax.y;
    if (range <= 0.0)
    {
        imageStore(u_heightfieldOut, coord, vec4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    float invalidNorm = shift ? clamp((-minmax.x) / range, 0.0, 1.0) : 0.0;
    float norm = invalidNorm;
    if (isValidFloat(h))
    {
        float shifted = shift ? (h - minmax.x) : h;
        norm = clamp(shifted / range, 0.0, 1.0);
    }

    float quantized = floor(norm * 65535.0) / 65535.0;
    imageStore(u_heightfieldOut, coord, vec4(quantized, 0.0, 0.0, 0.0));
}

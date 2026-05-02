$input a_position, i_data0, i_data1, i_data2, i_data3
$output v_color0, v_texcoord0, v_style

#include <bgfx_shader.sh>

SAMPLER2D(u_rectMaxSampler, 2);

uniform vec4 u_rectMaxParams;  // x: width, y: invWidth
uniform vec4 u_rectViewParams; // x: viewportW, y: viewportH, z: time, w: overlay Z lift

void main()
{
    float edgeId = a_position.x;
    float along = a_position.y;
    float side = a_position.z;

    vec4 rect0 = i_data0;
    vec4 rect1 = i_data1;
    vec4 style0 = i_data2;
    vec4 color = i_data3;

    float rectIndex = float(gl_InstanceID);
    float u = (rectIndex + 0.5) * u_rectMaxParams.y;
    float height = texture2DLod(u_rectMaxSampler, vec2(u, 0.5), 0.0).r;
    height += max(u_rectViewParams.w, 0.0);

    vec2 p00 = rect0.xy;
    vec2 uVec = rect0.zw;
    vec2 vVec = rect1.xy;

    vec3 p0 = vec3(p00, 0.0);
    vec3 p1 = vec3(p00 + uVec, 0.0);
    vec3 p3 = vec3(p00 + vVec, 0.0);
    vec3 p2 = vec3(p00 + uVec + vVec, 0.0);

    vec3 q0 = vec3(p00, height);
    vec3 q1 = vec3(p00 + uVec, height);
    vec3 q3 = vec3(p00 + vVec, height);
    vec3 q2 = vec3(p00 + uVec + vVec, height);

    vec3 a = p0;
    vec3 b = p1;

    int e = int(edgeId + 0.5);
    if (e == 0)
    {
        a = p0; b = p1;
    }
    else if (e == 1)
    {
        a = p1; b = p2;
    }
    else if (e == 2)
    {
        a = p2; b = p3;
    }
    else if (e == 3)
    {
        a = p3; b = p0;
    }
    else if (e == 4)
    {
        a = q0; b = q1;
    }
    else if (e == 5)
    {
        a = q1; b = q2;
    }
    else if (e == 6)
    {
        a = q2; b = q3;
    }
    else if (e == 7)
    {
        a = q3; b = q0;
    }
    else if (e == 8)
    {
        a = p0; b = q0;
    }
    else if (e == 9)
    {
        a = p1; b = q1;
    }
    else if (e == 10)
    {
        a = p2; b = q2;
    }
    else
    {
        a = p3; b = q3;
    }

    vec4 startClip = mul(u_modelViewProj, vec4(a, 1.0));
    vec4 endClip = mul(u_modelViewProj, vec4(b, 1.0));
    vec2 startNdc = startClip.xy / startClip.w;
    vec2 endNdc = endClip.xy / endClip.w;
    vec2 dirNdc = endNdc - startNdc;
    vec2 dirPixels = dirNdc * 0.5 * u_rectViewParams.xy;
    float lenPixels = length(dirPixels);

    float dirLen = length(dirNdc);
    vec2 dirNorm = dirLen > 0.0001 ? (dirNdc / dirLen) : vec2(1.0, 0.0);
    vec2 perp = vec2(-dirNorm.y, dirNorm.x);

    float lineWidth = rect1.z > 0.0 ? rect1.z : 1.0;
    float halfWidth = lineWidth * 0.5;
    vec2 offsetNdc = perp * (halfWidth * 2.0 / u_rectViewParams.xy);

    vec4 posClip = mix(startClip, endClip, along);
    posClip.xy += offsetNdc * side * posClip.w;

    gl_Position = posClip;
    v_color0 = color;
    v_texcoord0 = vec2(along * lenPixels, edgeId);
    v_style = vec4(rect1.w, style0.x, style0.y, style0.z);
}

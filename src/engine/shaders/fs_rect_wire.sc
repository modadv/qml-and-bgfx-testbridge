$input v_color0, v_texcoord0, v_style

#include <bgfx_shader.sh>

uniform vec4 u_rectViewParams;  // x: viewportW, y: viewportH, z: time, w: unused
uniform vec4 u_rectDebugParams; // x: debugAxes, y: dashX, z: dashY, w: dashZ

void main()
{
    float dashLen = v_style.x;
    float gapLen = v_style.y;
    vec4 color = v_color0;
    int edgeId = int(v_texcoord0.y + 0.5);

    if (edgeId < 4)
    {
        discard;
    }

    if (u_rectDebugParams.x > 0.5)
    {
        int axis = 2;
        if (edgeId == 0 || edgeId == 2 || edgeId == 4 || edgeId == 6)
        {
            axis = 0;
        }
        else if (edgeId == 1 || edgeId == 3 || edgeId == 5 || edgeId == 7)
        {
            axis = 1;
        }

        if (axis == 0)
        {
            color = vec4(1.0, 0.2, 0.2, 1.0);
            dashLen = u_rectDebugParams.y;
        }
        else if (axis == 1)
        {
            color = vec4(0.2, 1.0, 0.2, 1.0);
            dashLen = u_rectDebugParams.z;
        }
        else
        {
            color = vec4(0.2, 0.6, 1.0, 1.0);
            dashLen = u_rectDebugParams.w;
        }

        gapLen = dashLen * 0.5;
    }

    if (dashLen > 0.0 && gapLen > 0.0)
    {
        float cycle = dashLen + gapLen;
        float pos = mod(v_texcoord0.x, cycle);
        if (pos > dashLen)
            discard;
    }

    float blinkPeriod = v_style.z;
    float blinkDuty = v_style.w;
    if (blinkPeriod > 0.0 && blinkDuty > 0.0)
    {
        float phase = fract(u_rectViewParams.z / blinkPeriod);
        if (phase > blinkDuty)
            discard;
    }

    gl_FragColor = color;
}

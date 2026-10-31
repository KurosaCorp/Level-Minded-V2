// Colored line segments from LineBatch.

#include "Common.hlsli"

struct VSIn
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOut
{
    float4 position : SV_Position;
    float4 color    : COLOR;
};

VSOut VSMain(VSIn input)
{
    VSOut output;
    output.position = mul(float4(input.position, 1.0), g_ViewProj);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOut input) : SV_Target
{
    return input.color;
}

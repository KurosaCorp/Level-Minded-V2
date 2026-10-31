// Instanced solid meshes. One draw call per mesh type; per-instance data places, scales and tints
// each item. Simple editor lighting: key light + sky/ground ambient + a little headlight.

#include "Common.hlsli"

#define MESH_FLAG_SELECTED 1u
#define MESH_FLAG_EMISSIVE 2u

static const float3 kKeyLightDir = float3(0.3478, -0.4969, 0.7950); // normalized (0.35, -0.5, 0.8)
static const float3 kSelectionColor = float3(1.0, 0.63, 0.16);

struct VSIn
{
    float3 position      : POSITION;
    float3 normal        : NORMAL;
    float3 instPosition  : INSTPOS;
    float3 instScale     : INSTSCALE;
    float4 instColor     : INSTCOLOR;
    uint   instFlags     : INSTFLAGS;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    nointerpolation float3 color : COLOR;
    nointerpolation uint   flags : FLAGS;
};

VSOut VSMain(VSIn input)
{
    VSOut output;
    const float3 world = input.position * input.instScale + input.instPosition;
    output.position = mul(float4(world, 1.0), g_ViewProj);
    output.worldPos = world;
    // Inverse-transpose of an axis-aligned scale is 1 / scale.
    output.normal = input.normal / max(abs(input.instScale), 1e-4);
    output.color = input.instColor.rgb;
    output.flags = input.instFlags;
    return output;
}

float4 PSMain(VSOut input) : SV_Target
{
    const float3 n = normalize(input.normal);
    const float3 toCamera = normalize(g_CameraPos - input.worldPos);
    const float facing = saturate(dot(n, toCamera));

    float3 color;
    if (input.flags & MESH_FLAG_EMISSIVE) {
        color = input.color * (0.85 + 0.25 * facing);
    } else {
        const float key = saturate(dot(n, kKeyLightDir));
        const float ambient = lerp(0.30, 0.56, n.z * 0.5 + 0.5); // ground -> sky
        color = input.color * (ambient + key * 0.62 + facing * 0.16);
    }

    if (input.flags & MESH_FLAG_SELECTED) {
        const float rim = pow(1.0 - facing, 2.0);
        color = lerp(color, kSelectionColor, 0.35) + kSelectionColor * rim * 0.6;
    }
    return float4(color, 1.0);
}

// Instanced solid meshes. One draw call per mesh type; per-instance data places, rotates,
// scales and tints each item. Simple editor lighting: key light + sky/ground ambient + a little headlight.

#include "Common.hlsli"

#define MESH_FLAG_SELECTED 1u
#define MESH_FLAG_EMISSIVE 2u

static const float3 kKeyLightDir = float3(0.3478, -0.4969, 0.7950); // normalized (0.35, -0.5, 0.8)
static const float3 kSelectionColor = float3(1.0, 0.63, 0.16);

struct VSIn
{
    float3 position      : POSITION;
    float3 normal        : NORMAL;
    float4 vertexColor   : COLOR;     // material color of imported models, white for built-ins
    float3 instPosition  : INSTPOS;
    float4 instRotation  : INSTROT;   // unit quaternion
    float3 instScale     : INSTSCALE;
    float4 instColor     : INSTCOLOR;
    uint   instFlags     : INSTFLAGS;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float3 color : COLOR; // interpolated: imported models may have per-vertex colors
    nointerpolation uint   flags : FLAGS;
};

// Rotates v by the unit quaternion q.
float3 QuatRotate(float4 q, float3 v)
{
    const float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

VSOut VSMain(VSIn input)
{
    VSOut output;
    const float3 world = QuatRotate(input.instRotation, input.position * input.instScale) + input.instPosition;
    output.position = mul(float4(world, 1.0), g_ViewProj);
    output.worldPos = world;
    // Inverse-transpose of scale-then-rotate: divide by the scale, then rotate.
    output.normal = QuatRotate(input.instRotation, input.normal / max(abs(input.instScale), 1e-4) * sign(input.instScale));
    output.color = input.instColor.rgb * input.vertexColor.rgb;
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

// Unlit instance color, for instanced wireframes (selection outlines) drawn with the same vertex shader.
float4 WirePSMain(VSOut input) : SV_Target
{
    return float4(input.color, 1.0);
}

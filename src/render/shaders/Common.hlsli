#ifndef LM_COMMON_HLSLI
#define LM_COMMON_HLSLI

// Per-viewport constants. Must match FrameConstants in ViewportRenderer.cpp.
// World space is right-handed, Z-up. Depth is reversed (1 = near plane, 0 = infinitely far).
cbuffer FrameConstants : register(b0)
{
    row_major float4x4 g_ViewProj;
    row_major float4x4 g_InvViewProj;
    float3 g_CameraPos;
    float  g_GridFadeDistance; // world units; the grid fades out before this distance
    float2 g_ViewportOrigin;   // render-target pixels
    float2 g_ViewportSize;     // render-target pixels
    uint   g_GridFlags;        // bit 0: grid lines, bit 1: X/Y axis lines
    float3 g_Padding;
};

#define GRID_FLAG_LINES 1u
#define GRID_FLAG_AXES  2u

#endif

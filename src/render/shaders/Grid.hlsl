// Infinite ground grid on the z = 0 plane, drawn as one full-viewport triangle.
//
// Each pixel casts a view ray onto the plane and computes line coverage analytically, so the grid
// needs no geometry and costs one draw call at any extent. Spacing steps by powers of ten based on
// the on-screen footprint (1 m, 10 m, 100 m, ...), cross-fading between levels so lines never get
// denser than a few pixels apart, which prevents moire at distance and at grazing angles.

#include "Common.hlsli"

static const float  kBaseCellSize   = 1.0;  // world units of the finest grid level
static const float  kMinCellPixels  = 10.0; // finest level shown once cells are at least this big on screen
static const float  kMinorAlpha     = 0.22;
static const float  kMajorAlpha     = 0.50;
static const float3 kGridColor      = float3(0.55, 0.56, 0.60);
static const float3 kAxisXColor     = float3(0.90, 0.24, 0.27);
static const float3 kAxisYColor     = float3(0.42, 0.78, 0.20);

struct VSOut
{
    float4 position : SV_Position;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    // Triangle covering the whole viewport: (-1, 1), (3, 1), (-1, -3).
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOut output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// Coverage (0..1) of lines at integer coordinates, `widthPx` pixels wide, anti-aliased over ~1 pixel.
float LineCoverage(float2 coord, float2 unitsPerPixel, float widthPx)
{
    const float2 distancePx = abs(frac(coord + 0.5) - 0.5) / max(unitsPerPixel, 1e-7);
    const float2 coverage = saturate(widthPx * 0.5 + 0.5 - distancePx);
    return max(coverage.x, coverage.y);
}

struct PSOut
{
    float4 color : SV_Target;
    float  depth : SV_Depth;
};

PSOut PSMain(VSOut input)
{
    // Pixel -> NDC -> world-space point on the near plane -> view ray.
    const float2 ndc = (input.position.xy - g_ViewportOrigin) / g_ViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float4 nearPoint = mul(float4(ndc, 1.0, 1.0), g_InvViewProj);
    const float3 rayDir = normalize(nearPoint.xyz / nearPoint.w - g_CameraPos);

    // Ray / plane (z = 0). Keep going for misses so screen-space derivatives stay valid; alpha hides them.
    const float hitT = -g_CameraPos.z / rayDir.z;
    const bool hit = hitT > 0.0;
    const float3 worldPos = g_CameraPos + rayDir * (hit ? hitT : 0.0);
    const float2 uv = worldPos.xy;

    // World units covered by one pixel along X and Y.
    const float2 uvDx = ddx(uv);
    const float2 uvDy = ddy(uv);
    const float2 unitsPerPixel = float2(length(float2(uvDx.x, uvDy.x)), length(float2(uvDx.y, uvDy.y)));
    const float footprint = max(unitsPerPixel.x, unitsPerPixel.y);

    float3 color = kGridColor;
    float alpha = 0.0;

    if (g_GridFlags & GRID_FLAG_LINES) {
        const float lod = max(0.0, log10(footprint * kMinCellPixels / kBaseCellSize));
        const float lodLevel = floor(lod);
        const float lodBlend = lod - lodLevel; // 0 -> 1 while moving away

        const float cell0 = kBaseCellSize * pow(10.0, lodLevel);
        const float cell1 = cell0 * 10.0;
        const float cell2 = cell1 * 10.0;

        // Finest level fades out; middle level goes from major to minor; coarsest level stays major.
        const float a0 = LineCoverage(uv / cell0, unitsPerPixel / cell0, 1.0) * kMinorAlpha * (1.0 - lodBlend);
        const float a1 = LineCoverage(uv / cell1, unitsPerPixel / cell1, lerp(1.5, 1.0, lodBlend)) * lerp(kMajorAlpha, kMinorAlpha, lodBlend);
        const float a2 = LineCoverage(uv / cell2, unitsPerPixel / cell2, 1.5) * kMajorAlpha;
        alpha = max(a0, max(a1, a2));
    }

    if (g_GridFlags & GRID_FLAG_AXES) {
        const float2 axisDistancePx = abs(uv) / max(unitsPerPixel, 1e-7);
        const float xAxis = saturate(1.5 - axisDistancePx.y); // line y = 0, 2 px wide
        const float yAxis = saturate(1.5 - axisDistancePx.x); // line x = 0
        color = lerp(color, kAxisXColor, xAxis);
        color = lerp(color, kAxisYColor, yAxis);
        alpha = max(alpha, max(xAxis, yAxis) * 0.95);
    }

    // Fade toward the horizon.
    const float distance = length(worldPos - g_CameraPos);
    alpha *= 1.0 - smoothstep(g_GridFadeDistance * 0.35, g_GridFadeDistance, distance);
    alpha *= hit ? 1.0 : 0.0;
    clip(alpha - 1.0 / 255.0);

    const float4 clipPos = mul(float4(worldPos, 1.0), g_ViewProj);
    PSOut output;
    output.color = float4(color, alpha);
    output.depth = clipPos.z / clipPos.w;
    return output;
}

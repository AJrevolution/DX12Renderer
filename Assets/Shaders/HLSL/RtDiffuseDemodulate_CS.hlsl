#include "Common.hlsli"

Texture2D<float4> g_DiffuseRadiance : register(t0);
Texture2D<float4> g_DiffuseAlbedo : register(t1);
Texture2D<float> g_Depth : register(t2);

RWTexture2D<float4> g_DiffuseLighting : register(u0);
RWTexture2D<float4> g_Output : register(u1);

cbuffer DiffuseDemodulateConstants : register(b0)
{
    uint DebugView;
    uint3 _pad0;
};

static const float DEMOD_EPS = 1e-3f;
static const float ALBEDO_STABLE_MIN = 0.03f;
static const float ALBEDO_STABLE_MAX = 0.97f;

bool AlbedoStable(float3 albedo)
{
    float minA = min(albedo.r, min(albedo.g, albedo.b));
    float maxA = max(albedo.r, max(albedo.g, albedo.b));

    return minA > ALBEDO_STABLE_MIN &&
           maxA < ALBEDO_STABLE_MAX;
}

float3 ToneMapForDebug(float3 x)
{
    return x / (1.0f.xxx + x);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_DiffuseLighting.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float4 radiance = g_DiffuseRadiance[pixel];
    float3 albedo = saturate(g_DiffuseAlbedo[pixel].rgb);
    float depth = g_Depth[pixel];

    const bool hasStableSurfaceAlbedo =
        g_DiffuseAlbedo[pixel].a > 0.5f &&
        AlbedoStable(albedo);

    const bool sky = depth >= 0.9999f;

    float3 lighting = 0.0f.xxx;

    if (sky || !hasStableSurfaceAlbedo)
    {
        // Preserve sky/background and unstable-albedo surface radiance.
        // Combine will skip remodulation when albedo alpha is 0.
        lighting = radiance.rgb;
    }
    else
    {
        lighting = radiance.rgb / max(albedo, DEMOD_EPS.xxx);
    }

    g_DiffuseLighting[pixel] = float4(lighting, radiance.a);

    if (DebugView == 69u)
    {
        float3 vis = ToneMapForDebug(max(0.0f.xxx, lighting));
        g_Output[pixel] = float4(LinearToSRGB(vis), 1.0f);
    }
    else if (DebugView == 70u)
    {
        float v = (sky || !hasStableSurfaceAlbedo) ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
}

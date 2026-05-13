#include "Common.hlsli"

Texture2D<float4> g_StableHistory : register(t0);
Texture2D<float4> g_RespHistory : register(t1);
Texture2D<float4> g_GuideNormal : register(t2);
Texture2D<float> g_GuideDepth : register(t3);

RWTexture2D<float4> g_SelectedSignal : register(u0); // linear, feeds SVGF/A-Trous
RWTexture2D<float4> g_Output : register(u1); // display

cbuffer RtHistorySelectConstants : register(b0)
{
    float RoughnessThreshold;
    float RoughnessRange;
    uint DebugView;
    uint _pad0;
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 stable = g_StableHistory[pixel].rgb;
    float3 resp = g_RespHistory[pixel].rgb;

    float4 guide = g_GuideNormal[pixel];
    float roughness = guide.a;
    float depth = g_GuideDepth[pixel];

    float t = saturate((RoughnessThreshold - roughness) / max(RoughnessRange, 1e-4f));

    // Sky / miss path stays on stable.
    if (depth >= 0.9999f)
        t = 0.0f;

    float3 selected = lerp(stable, resp, t);
    
    g_SelectedSignal[pixel] = float4(selected, 1.0f);

    float3 display = selected;
    if (DebugView == 29)
        display = t.xxx;
    else if (DebugView == 30)
        display = stable;
    else if (DebugView == 31)
        display = resp;

    g_Output[pixel] = float4(LinearToSRGB(display), 1.0f);
}
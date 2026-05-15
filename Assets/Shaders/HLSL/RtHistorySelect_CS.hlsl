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
    float LengthBias;
    float LengthScale;
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

    float4 stable4 = g_StableHistory[pixel];
    float4 resp4 = g_RespHistory[pixel];

    float3 stable = stable4.rgb;
    float3 resp = resp4.rgb;

    float stableLen = stable4.a;
    float respLen = resp4.a;

    float4 guide = g_GuideNormal[pixel];
    float roughness = guide.a;
    float depth = g_GuideDepth[pixel];
    
    float tRough = saturate(
    (RoughnessThreshold - roughness) / max(RoughnessRange, 1e-4f));

    float lenDelta = (respLen - stableLen) + LengthBias;
    float tLen = saturate(0.5f + lenDelta * LengthScale);
    
    // Roughness remains the material-intent base. Length can pull the decision
    // toward the history that has accumulated more trustworthy samples.
    float t = lerp(tRough, tLen, 0.5f);

    // Sky / miss path stays on stable.
    if (depth >= 0.9999f)
        t = 0.0f;

    float3 selected = lerp(stable, resp, t);
    float selectedLen = lerp(stableLen, respLen, t);
    
    g_SelectedSignal[pixel] = float4(selected, selectedLen);

    float3 display = selected;
    if (DebugView == 29)
        display = t.xxx;
    else if (DebugView == 30)
        display = stable;
    else if (DebugView == 31)
        display = resp;
    else if (DebugView == 37)
    {
        display = tRough.xxx;
    }
    else if (DebugView == 38)
    {
        display = tLen.xxx;
    }
    else if (DebugView == 39)
    {
        display = t.xxx;
    }
    else if (DebugView == 40)
    {
        display = (stableLen / 255.0f).xxx;
    }
    else if (DebugView == 41)
    {
        display = (respLen / 255.0f).xxx;
    }

    g_Output[pixel] = float4(LinearToSRGB(display), 1.0f);
}
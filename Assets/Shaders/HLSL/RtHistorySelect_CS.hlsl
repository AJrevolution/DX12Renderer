#include "Common.hlsli"

// RT DebugView ownership for this pass:
// History select owns:
//   29 = final selector mask
//   30 = stable history signal
//   31 = responsive history signal
//   37 = roughness selector vote
//   38 = length selector vote
//   39 = final selector value
//   40 = stable history length
//   41 = responsive history length
//   42 = selected history length

Texture2D<float4> g_StableHistory : register(t0);
Texture2D<float4> g_RespHistory : register(t1);
Texture2D<float4> g_GuideNormal : register(t2);
Texture2D<float> g_GuideDepth : register(t3);
Texture2D<float> g_MotionConf : register(t4);

RWTexture2D<float4> g_SelectedSignal : register(u0); // linear, feeds SVGF/A-Trous
RWTexture2D<float4> g_Output : register(u1); // display

cbuffer RtHistorySelectConstants : register(b0)
{
    float RoughnessThreshold;
    float RoughnessRange;
    float LengthBias;
    float LengthScale;
    float LengthInfluence;
    float MotionTrustInfluence;
    float MotionConfMin;
    float MotionConfPower;
    uint DebugView;
    uint3 _pad0;
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
    
    float motionConfRaw = saturate(g_MotionConf[pixel]);
    float motionConfW = pow(motionConfRaw, max(MotionConfPower, 1e-3f));
    
    // Roughness vote:
    // Low roughness / glossy -> responsive.
    // High roughness -> stable.
    float tRough = saturate(
    (RoughnessThreshold - roughness) / max(RoughnessRange, 1e-4f));

    // Length vote:
    // Positive value means responsive history is healthier/longer.
    // Negative value means stable history is healthier/longer.
    float lenDelta = (respLen - stableLen) * max(0.0f, LengthScale) + LengthBias;

    // Keep the length vote bounded so an aggressive LengthScale does not make
    // history length dominate roughness/motion selection instantly.
    float lenAdjust = clamp(lenDelta, -0.5f, 0.5f);
    float tLen = saturate(0.5f + lenAdjust);
    
    // When current motion confidence is weak, do not allow "long stable history"
    // to dominate as strongly. This prevents stale stable spec from clinging
    // through disocclusions or uncertain reprojection.
    float lengthInfluence = saturate(LengthInfluence) * motionConfW;
    float tBase = lerp(tRough, tLen, lengthInfluence);
    
    // Motion trust vote:
    // Low confidence pushes toward responsive. This is deliberately one knob;
    // threshold/power are reused from the spec temporal policy.
    float motionTrustInfluence = saturate(MotionTrustInfluence);
    float tMotion = lerp(tBase, 1.0f, motionTrustInfluence * (1.0f - motionConfW));

    float t = tMotion;

    // Hard gate: below spec motion-confidence threshold, use responsive.
    if (motionConfRaw < saturate(MotionConfMin))
    {
        t = 1.0f;
    }
    
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
        display = saturate(stableLen / 255.0f).xxx;
    }
    else if (DebugView == 41)
    {
        display = saturate(respLen / 255.0f).xxx;
    }
    else if (DebugView == 42)
    {
        display = saturate(selectedLen / 255.0f).xxx;
    }

    g_Output[pixel] = float4(LinearToSRGB(display), 1.0f);
}
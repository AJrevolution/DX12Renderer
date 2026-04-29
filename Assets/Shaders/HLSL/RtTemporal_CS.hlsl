#include "Common.hlsli"

Texture2D<float4> g_CurrAccum : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<float4> g_PrevAccum : register(t3);
Texture2D<float4> g_PrevNormal : register(t4);
Texture2D<float> g_PrevDepth : register(t5);
Texture2D<float2> g_PrevMoments : register(t6);

RWTexture2D<float4> g_HistoryOut : register(u0); // linear
RWTexture2D<float4> g_Output : register(u1); // display
RWTexture2D<float2> g_MomentsOut : register(u2);

SamplerState g_LinearClamp : register(s0);

cbuffer RtTemporalConstants : register(b0)
{
    row_major float4x4 CurrInvViewProj;
    row_major float4x4 PrevViewProj;

    float2 InvResolution;
    float TemporalAlpha;
    float DepthSigma;
    float NormalSigma;

    uint TemporalEnabled;
    uint HistoryValid;
    uint DebugView;
    uint _pad0;
};

float3 UnpackNormal(float4 packed)
{
    float3 n = packed.xyz * 2.0f - 1.0f;
    return SafeNormalize(n);
}

float3 ReconstructWorldPos(uint2 pixel, float depth01)
{
    float2 uv = (float2(pixel) + 0.5f) * InvResolution;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 clip = float4(ndc, depth01, 1.0f);
    float4 world = mul(clip, CurrInvViewProj);
    return world.xyz / max(1e-6f, world.w);
}

float2 ProjectPrevUV(float3 worldPos)
{
    float4 prevClip = mul(float4(worldPos, 1.0f), PrevViewProj);
    float2 prevNdc = prevClip.xy / max(1e-6f, prevClip.w);
    return float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
}

void NeighborhoodMinMax(uint2 pixel, out float3 cmin, out float3 cmax)
{
    uint width, height;
    g_CurrAccum.GetDimensions(width, height);

    cmin = float3(1e20f, 1e20f, 1e20f);
    cmax = float3(-1e20f, -1e20f, -1e20f);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 p = int2(pixel) + int2(x, y);
            p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float3 c = g_CurrAccum[uint2(p)].rgb;
            cmin = min(cmin, c);
            cmax = max(cmax, c);
        }
    }
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 currColor = g_CurrAccum[pixel].rgb;
    float3 currNormal = UnpackNormal(g_CurrNormal[pixel]);
    float currDepth = g_CurrDepth[pixel];

    bool validReuse = false;
    float2 prevUV = 0.0f.xx;
    float3 prevColor = 0.0f.xxx;
    float3 prevNormal = 0.0f.xxx;
    float prevDepth = 1.0f;
    float prevLen = 0.0f;
    float2 prevMoments = 0.0f.xx;

    if (TemporalEnabled != 0 && HistoryValid != 0 && currDepth < 0.9999f)
    {
        float3 worldPos = ReconstructWorldPos(pixel, currDepth);
        prevUV = ProjectPrevUV(worldPos);

        bool inBounds =
            prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
            prevUV.y >= 0.0f && prevUV.y <= 1.0f;

        if (inBounds)
        {
            prevColor = g_PrevAccum.SampleLevel(g_LinearClamp, prevUV, 0.0f).rgb;
            prevLen = g_PrevAccum.SampleLevel(g_LinearClamp, prevUV, 0.0f).a;
            prevNormal = UnpackNormal(g_PrevNormal.SampleLevel(g_LinearClamp, prevUV, 0.0f));
            prevDepth = g_PrevDepth.SampleLevel(g_LinearClamp, prevUV, 0.0f);
            prevMoments = g_PrevMoments.SampleLevel(g_LinearClamp, prevUV, 0.0f);
            
            float nd = saturate(dot(currNormal, prevNormal));
            bool normalOk = nd > (1.0f - NormalSigma);
            bool depthOk = abs(currDepth - prevDepth) < DepthSigma;

            validReuse = normalOk && depthOk;
        }
    }

    float3 currMin, currMax;
    NeighborhoodMinMax(pixel, currMin, currMax);
    float3 prevClamped = clamp(prevColor, currMin, currMax);
    
    float newLen = validReuse ? min(prevLen + 1.0f, 255.0f) : 1.0f;

    float alphaUsed = 0.0f;
    float3 history = currColor;
    
    float currLuma = Luminance(currColor);
    float2 currMoments = float2(currLuma, currLuma * currLuma);
    float2 moments = currMoments;
    
    if (validReuse)
    {
        float k = saturate(newLen / 8.0f);
        alphaUsed = lerp(0.25f, TemporalAlpha, k);
        history = lerp(currColor, prevClamped, alphaUsed);
        moments = lerp(currMoments, prevMoments, alphaUsed);
    }
    
    float variance = max(moments.y - moments.x * moments.x, 0.0f);
    
    float3 display = history;

    if (DebugView == 18)
    {
        display = validReuse ? 1.0f.xxx : 0.0f.xxx;
    }
    else if (DebugView == 19)
    {
        display = validReuse ? 0.0f.xxx : 1.0f.xxx;
    }
    else if (DebugView == 20)
    {
        display = float3(prevUV, 0.0f);
    }
    else if (DebugView == 21)
    {
        display = saturate(abs(prevClamped - currColor) * 4.0f);
    }
    else if (DebugView == 22)
    {
        display = (newLen / 255.0f).xxx;            // accumulated history length
    }
    else if (DebugView == 23)
    {
        display = alphaUsed.xxx;                    // final blend weight actually used
    }
    else if (DebugView == 24)
    {
        display = saturate(variance * 16.0f).xxx;   // variance / noise estimate
    }
    else if (DebugView == 25)
    {
        float k = saturate(newLen / 8.0f);
        display = k.xxx;                            // history warm-up / convergence factor
    }
    else if (DebugView == 26)
    {
        float reprojErr = 0.0f;

        if (validReuse)
            reprojErr = saturate(abs(currDepth - prevDepth) / max(1e-4f, DepthSigma));
        else
            reprojErr = 1.0f;

        display = reprojErr.xxx;                   // depth mismatch / rejection pressure
    }
    
    g_HistoryOut[pixel] = float4(history, newLen);
    g_MomentsOut[pixel] = moments;
    
    if (DebugView >= 18 && DebugView <= 26)
        g_Output[pixel] = float4(display, 1.0f);
    else
        g_Output[pixel] = float4(LinearToSRGB(history), 1.0f);
}
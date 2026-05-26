#include "Common.hlsli"

Texture2D<float2> g_RawPrevUV : register(t0);
Texture2D<float4> g_AovNormal : register(t1);
Texture2D<float> g_AovDepth : register(t2);

RWTexture2D<float2> g_DilatedPrevUV : register(u0);
RWTexture2D<float4> g_Output : register(u1);

cbuffer MotionDilateConstants : register(b0)
{
    float2 InvResolution;
    uint Radius;
    float DepthSigma;

    float NormalSigma;
    float MinScore;
    uint DebugView;
    uint _pad0;
};

bool PrevUVValid(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

float3 DecodeGuideNormal(float4 packedNormalRoughness)
{
    return SafeNormalize(packedNormalRoughness.xyz * 2.0f - 1.0f);
}

void WriteDebugMask(uint2 pixel, bool invalidAfterDilation)
{
    if (DebugView == 54u)
    {
        float v = invalidAfterDilation ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_DilatedPrevUV.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float2 rawPrevUV = g_RawPrevUV[pixel];

    if (PrevUVValid(rawPrevUV))
    {
        g_DilatedPrevUV[pixel] = rawPrevUV;
        WriteDebugMask(pixel, false);
        return;
    }

    float centerDepth = g_AovDepth[pixel];

    if (centerDepth >= 0.9999f)
    {
        g_DilatedPrevUV[pixel] = float2(-1.0f, -1.0f);
        WriteDebugMask(pixel, true);
        return;
    }

    float3 centerNormal = DecodeGuideNormal(g_AovNormal[pixel]);

    float bestScore = 0.0f;
    float2 bestPrevUV = float2(-1.0f, -1.0f);

    const int r = int(Radius);

    for (int y = -r; y <= r; ++y)
    {
        for (int x = -r; x <= r; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            int2 q = int2(pixel) + int2(x, y);
            q = clamp(q, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float2 candidatePrevUV = g_RawPrevUV[q];

            if (!PrevUVValid(candidatePrevUV))
                continue;

            float candidateDepth = g_AovDepth[q];

            if (candidateDepth >= 0.9999f)
                continue;

            float3 candidateNormal = DecodeGuideNormal(g_AovNormal[q]);

            float depthDelta = abs(centerDepth - candidateDepth);
            float depthWeight = exp(-depthDelta / max(1e-5f, DepthSigma));

            float normalDot = saturate(dot(centerNormal, candidateNormal));
            float normalWeight = exp(-(1.0f - normalDot) / max(1e-5f, NormalSigma));

            float spatialDist2 = float(x * x + y * y);
            float spatialSigma = max(1.0f, float(Radius));
            float spatialWeight = exp(-spatialDist2 / max(1e-5f, 2.0f * spatialSigma * spatialSigma));

            float score = depthWeight * normalWeight * spatialWeight;

            if (score > bestScore)
            {
                bestScore = score;
                bestPrevUV = candidatePrevUV;
            }
        }
    }

    bool validAfterDilation = bestScore >= MinScore;

    float2 outPrevUV = validAfterDilation
        ? bestPrevUV
        : float2(-1.0f, -1.0f);

    g_DilatedPrevUV[pixel] = outPrevUV;
    WriteDebugMask(pixel, !validAfterDilation);
}

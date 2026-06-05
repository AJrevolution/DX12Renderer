#include "Common.hlsli"

Texture2D<float2> g_RawPrevUV : register(t0);
Texture2D<float4> g_AovNormal : register(t1);
Texture2D<float> g_AovDepth : register(t2);
Texture2D<float> g_ViewZ : register(t3);
Texture2D<uint> g_SurfaceId : register(t4);

RWTexture2D<float2> g_DilatedPrevUV : register(u0);
RWTexture2D<float> g_MotionConf : register(u1);
RWTexture2D<float4> g_Output : register(u2);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

cbuffer MotionDilateConstants : register(b0)
{
    float2 InvResolution;
    uint Radius;
    float DepthSigma;

    float NormalSigma;
    float MinScore;
    uint DebugView;
    uint _pad0;
    
    float3 DistanceNormParams;
    float DistanceNormSigma;
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

void WriteDebug(uint2 pixel, bool invalidAfterDilation, float confidence)
{
    if (DebugView == 54u)
    {
        float v = invalidAfterDilation ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
    else if (DebugView == 55u)
    {
        float v = saturate(confidence);
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_DilatedPrevUV.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    uint centerSurfaceId = g_SurfaceId[pixel];

    if (!SurfaceIdValid(centerSurfaceId))
    {
        g_DilatedPrevUV[pixel] = float2(-1.0f, -1.0f);
        g_MotionConf[pixel] = 0.0f;
        WriteDebug(pixel, true, 0.0f);
        return;
    }

    float2 rawPrevUV = g_RawPrevUV[pixel];

    if (PrevUVValid(rawPrevUV))
    {
        g_DilatedPrevUV[pixel] = rawPrevUV;
        g_MotionConf[pixel] = 1.0f;
        WriteDebug(pixel, false, 1.0f);
        return;
    }

    float centerDepth = g_AovDepth[pixel];

    if (centerDepth >= 0.9999f)
    {
        g_DilatedPrevUV[pixel] = float2(-1.0f, -1.0f);
        g_MotionConf[pixel] = 0.0f;
        WriteDebug(pixel, true, 0.0f);
        return;
    }

    float4 centerNormalRough = g_AovNormal[pixel];
    float3 centerNormal = DecodeGuideNormal(centerNormalRough);
    float centerRoughness = centerNormalRough.a;
    float centerViewZ = g_ViewZ[pixel];
    
    float centerNormZ =
    DistanceValid(centerViewZ)
        ? NormalizeDistance(centerViewZ, centerViewZ, centerRoughness, DistanceNormParams)
        : 0.0f;

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

            uint candidateSurfaceId = g_SurfaceId[q];

            if (candidateSurfaceId != centerSurfaceId)
                continue;
            
            float2 candidatePrevUV = g_RawPrevUV[q];

            if (!PrevUVValid(candidatePrevUV))
                continue;

            float candidateDepth = g_AovDepth[q];

            if (candidateDepth >= 0.9999f)
                continue;

            float3 candidateNormal = DecodeGuideNormal(g_AovNormal[q]);
            
            float candidateViewZ = g_ViewZ[q];
            
            float wViewZ = 1.0f;

            if (centerRoughness < 0.35f)
            {
                if (DistanceValid(centerViewZ) && DistanceValid(candidateViewZ))
                {
                    float candidateNormZ =
                        NormalizeDistance(candidateViewZ, centerViewZ, centerRoughness, DistanceNormParams);

                    wViewZ =
                        DistanceSimilarityWeight(centerNormZ, candidateNormZ, DistanceNormSigma);
                }
                else
                {
                    wViewZ = 0.0f;
                }
            }

            float depthDelta = abs(centerDepth - candidateDepth);
            float depthWeight = exp(-depthDelta / max(1e-5f, DepthSigma));

            float normalDot = saturate(dot(centerNormal, candidateNormal));
            float normalWeight = exp(-(1.0f - normalDot) / max(1e-5f, NormalSigma));

            float spatialDist2 = float(x * x + y * y);
            float spatialSigma = max(1.0f, float(Radius));
            float spatialWeight = exp(-spatialDist2 / max(1e-5f, 2.0f * spatialSigma * spatialSigma));

            float score = depthWeight * normalWeight * spatialWeight * wViewZ;

            if (score > bestScore)
            {
                bestScore = score;
                bestPrevUV = candidatePrevUV;
            }
        }
    }
    
    float confidence = saturate(bestScore);
    bool validAfterDilation = bestScore >= MinScore;

    float2 outPrevUV = validAfterDilation
        ? bestPrevUV
        : float2(-1.0f, -1.0f);

    if (!validAfterDilation)
        confidence = 0.0f;
    
    g_DilatedPrevUV[pixel] = outPrevUV;
    g_MotionConf[pixel] = confidence;
    WriteDebug(pixel, !validAfterDilation, confidence);
}

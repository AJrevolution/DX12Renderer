#include "Common.hlsli"

Texture2D<float> g_RawHitDist : register(t0);
Texture2D<float4> g_CurrNormalRough : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<float2> g_RawPrevUV : register(t3);
Texture2D<float> g_PrevHitDist : register(t4);
Texture2D<float> g_PrevDepth : register(t5);
Texture2D<float4> g_PrevNormalRough : register(t6);

RWTexture2D<float> g_ReconsHitDist : register(u0);
RWTexture2D<float> g_ReconsConf : register(u1);
RWTexture2D<float4> g_Output : register(u2);

cbuffer HitDistReconstructConstants : register(b0)
{
    float2 InvResolution;
    float Alpha;
    float DepthSigma;

    float NormalSigma;
    float RoughnessSigma;
    uint HistoryValid;
    uint DebugView;

    uint Radius;
    float HitDistVisMax;
    uint2 _pad0;
};

static const float HIT_DIST_INVALID = -1.0f;

bool HitDistValid(float d)
{
    return d >= 0.0f;
}

bool PrevUVValid(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

float3 DecodeNormal(float4 packedNormalRoughness)
{
    return SafeNormalize(packedNormalRoughness.xyz * 2.0f - 1.0f);
}

float GuideScore(
    float depth0,
    float depth1,
    float3 normal0,
    float3 normal1,
    float rough0,
    float rough1)
{
    if (depth0 >= 0.9999f || depth1 >= 0.9999f)
        return 0.0f;

    float depthW = exp(-abs(depth0 - depth1) / max(1e-5f, DepthSigma));

    float normalDot = saturate(dot(normal0, normal1));
    float normalW = exp(-(1.0f - normalDot) / max(1e-5f, NormalSigma));

    float roughW = exp(-abs(rough0 - rough1) / max(1e-5f, RoughnessSigma));

    return depthW * normalW * roughW;
}

void WriteDebug(uint2 pixel, float hitDist, float conf)
{
    if (DebugView == 63u)
    {
        float v = HitDistValid(hitDist)
            ? saturate(hitDist / max(1e-4f, HitDistVisMax))
            : 0.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
    else if (DebugView == 64u)
    {
        g_Output[pixel] = float4(saturate(conf).xxx, 1.0f);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_ReconsHitDist.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float rawHit = g_RawHitDist[pixel];

    float4 currNR = g_CurrNormalRough[pixel];
    float3 currN = DecodeNormal(currNR);
    float currRough = currNR.a;
    float currDepth = g_CurrDepth[pixel];

    float outHit = HIT_DIST_INVALID;
    float outConf = 0.0f;

    const bool rawValid = HitDistValid(rawHit) && currDepth < 0.9999f;

    if (rawValid)
    {
        outHit = rawHit;
        outConf = 1.0f;
    }

    // Temporal stabilization from previous reconstructed guide.
    float2 prevUV = g_RawPrevUV[pixel];

    if (HistoryValid != 0u &&
        PrevUVValid(prevUV) &&
        currDepth < 0.9999f)
    {
        int2 prevPixel = int2(prevUV * float2(width, height));
        prevPixel = clamp(prevPixel, int2(0, 0), int2(int(width) - 1, int(height) - 1));

        float prevHit = g_PrevHitDist[prevPixel];

        if (HitDistValid(prevHit))
        {
            float prevDepth = g_PrevDepth[prevPixel];
            float4 prevNR = g_PrevNormalRough[prevPixel];
            float3 prevN = DecodeNormal(prevNR);
            float prevRough = prevNR.a;

            float score = GuideScore(
                currDepth,
                prevDepth,
                currN,
                prevN,
                currRough,
                prevRough);

            if (score > 0.10f)
            {
                float histConf = saturate(score);

                if (HitDistValid(outHit))
                {
                    // Alpha is the current-sample weight. Low alpha gives stable history;
                    // high alpha follows current raw guide more aggressively.
                    float currentWeight = saturate(Alpha);
                    outHit = lerp(prevHit, outHit, currentWeight);
                    outConf = max(outConf, histConf);
                }
                else
                {
                    outHit = prevHit;
                    outConf = max(outConf, histConf * 0.85f);
                }
            }
        }
    }

    // Current-frame hole fill from raw neighboring hit distances.
    if ((!HitDistValid(outHit) || outConf < 0.35f) &&
        currDepth < 0.9999f)
    {
        float bestScore = 0.0f;
        float bestHit = HIT_DIST_INVALID;

        const int r = int(Radius);

        for (int y = -r; y <= r; ++y)
        {
            for (int x = -r; x <= r; ++x)
            {
                if (x == 0 && y == 0)
                    continue;

                int2 q = int2(pixel) + int2(x, y);
                q = clamp(q, int2(0, 0), int2(int(width) - 1, int(height) - 1));

                float candidateHit = g_RawHitDist[q];

                if (!HitDistValid(candidateHit))
                    continue;

                float qDepth = g_CurrDepth[q];

                if (qDepth >= 0.9999f)
                    continue;

                float4 qNR = g_CurrNormalRough[q];
                float3 qN = DecodeNormal(qNR);
                float qRough = qNR.a;

                float guideScore = GuideScore(
                    currDepth,
                    qDepth,
                    currN,
                    qN,
                    currRough,
                    qRough);

                float spatialDist2 = float(x * x + y * y);
                float sigmaSpatial = max(1.0f, float(Radius));
                float spatialW = exp(-spatialDist2 / max(1e-5f, 2.0f * sigmaSpatial * sigmaSpatial));

                float score = guideScore * spatialW;

                if (score > bestScore)
                {
                    bestScore = score;
                    bestHit = candidateHit;
                }
            }
        }

        if (bestScore > 0.05f && HitDistValid(bestHit))
        {
            if (HitDistValid(outHit))
            {
                float fillW = saturate((1.0f - outConf) * bestScore);
                outHit = lerp(outHit, bestHit, fillW);
                outConf = max(outConf, bestScore * 0.65f);
            }
            else
            {
                outHit = bestHit;
                outConf = bestScore * 0.65f;
            }
        }
    }

    if (currDepth >= 0.9999f)
    {
        outHit = HIT_DIST_INVALID;
        outConf = 0.0f;
    }

    g_ReconsHitDist[pixel] = HitDistValid(outHit) ? outHit : HIT_DIST_INVALID;
    g_ReconsConf[pixel] = saturate(outConf);

    WriteDebug(pixel, outHit, outConf);
}

#include "Common.hlsli"

// RT ViewZ reconstruction
// 63 = reconstructed ViewZ heatmap
// 64 = reconstructed ViewZ confidence
// 79 = normalized reconstructed ViewZ comparison-space value
// 80 = invalid normalized ViewZ mask

Texture2D<float> g_RawViewZ : register(t0);
Texture2D<float4> g_CurrNormalRough : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<float2> g_RawPrevUV : register(t3);
Texture2D<float> g_PrevViewZ : register(t4);
Texture2D<float> g_PrevDepth : register(t5);
Texture2D<float4> g_PrevNormalRough : register(t6);
Texture2D<uint> g_SurfaceId : register(t7);

RWTexture2D<float> g_ViewZRecons : register(u0);
RWTexture2D<float> g_ViewZReconsConf : register(u1);
RWTexture2D<float4> g_Output : register(u2);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;
static const float VIEWZ_INVALID = DISTANCE_INVALID;

cbuffer RtViewZReconstructConstants : register(b0)
{
    float2 InvResolution;
    float Alpha;
    float DepthSigma;

    float NormalSigma;
    float RoughnessSigma;
    uint HistoryValid;
    uint DebugView;

    uint Radius;
    float ViewZVisMax;
    uint2 _pad0;
    
    float3 DistanceNormParams;  // ViewZRaw / ViewZRecons invalid = -1.
    float DistanceNormSigma;    // ViewZReconsConf invalid / unknown = 0.
};

bool ViewZValid(float z)
{
    return DistanceValid(z);
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

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float CenterNormViewZ(float centerViewZ, float roughness)
{
    return NormalizeDistance(
        centerViewZ,
        centerViewZ,
        roughness,
        DistanceNormParams);
}

float CandidateNormViewZ(float candidateViewZ, float centerViewZ, float roughness)
{
    return NormalizeDistance(
        candidateViewZ,
        centerViewZ,
        roughness,
        DistanceNormParams);
}

float ViewZSimilarityWeight(
    float centerViewZ,
    float candidateViewZ,
    float centerRoughness)
{
    if (!ViewZValid(candidateViewZ))
        return 0.0f;

    // Important for reconstruction:
    // if the center has no ViewZ yet, do not block history/spatial hole fill.
    if (!ViewZValid(centerViewZ))
        return 1.0f;

    float n0 = CenterNormViewZ(centerViewZ, centerRoughness);
    float n1 = CandidateNormViewZ(candidateViewZ, centerViewZ, centerRoughness);

    return DistanceSimilarityWeight(n0, n1, DistanceNormSigma);
}

void WriteDebug(uint2 pixel, float viewZ, float conf, float roughness)
{
    if (DebugView == 63u)
    {
        float v = ViewZValid(viewZ)
            ? saturate(viewZ / max(1e-4f, ViewZVisMax))
            : 0.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
    else if (DebugView == 64u)
    {
        g_Output[pixel] = float4(saturate(conf).xxx, 1.0f);
    }
    
    else if (DebugView == 79u)
    {
        float v = 0.0f;

        if (ViewZValid(viewZ))
        {
        // Contract debug: visualize the same normalized distance space used by
        // ViewZ reconstruction / temporal / A-Trous comparison logic.
            v = NormalizeDistance(
            viewZ,
            viewZ,
            roughness,
            DistanceNormParams);
        }

        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
    
    else if (DebugView == 80u)
    {
        float v = ViewZValid(viewZ) ? 0.0f : 1.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_ViewZRecons.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float rawViewZ = g_RawViewZ[pixel];

    float4 currNR = g_CurrNormalRough[pixel];
    float3 currN = DecodeNormal(currNR);
    float currRough = currNR.a;
    float currDepth = g_CurrDepth[pixel];
    uint centerSurfaceId = g_SurfaceId[pixel];
    bool centerSurfaceValid = SurfaceIdValid(centerSurfaceId);
    
    

    float outViewZ = VIEWZ_INVALID;
    float outConf = 0.0f;

    const bool rawValid =
        centerSurfaceValid &&
        ViewZValid(rawViewZ) &&
        currDepth < 0.9999f;

    if (rawValid)
    {
        outViewZ = rawViewZ;
        outConf = 1.0f;
    }

    // Temporal stabilization from previous reconstructed guide.
    float2 prevUV = g_RawPrevUV[pixel];

    if (centerSurfaceValid &&
        HistoryValid != 0u &&
        PrevUVValid(prevUV) &&
        (!ViewZValid(outViewZ) || outConf < 0.35f) &&
        currDepth < 0.9999f)
    {
        int2 prevPixel = int2(prevUV * float2(width, height));
        prevPixel = clamp(prevPixel, int2(0, 0), int2(int(width) - 1, int(height) - 1));

        float prevViewZ = g_PrevViewZ[prevPixel];

        if (ViewZValid(prevViewZ))
        {
            float prevDepth = g_PrevDepth[prevPixel];
            float4 prevNR = g_PrevNormalRough[prevPixel];
            float3 prevN = DecodeNormal(prevNR);
            float prevRough = prevNR.a;

            float guideScore = GuideScore(
                currDepth,
                prevDepth,
                currN,
                prevN,
                currRough,
                prevRough);

            float centerCompareViewZ = outViewZ;

            float wViewZ = ViewZSimilarityWeight(
                centerCompareViewZ,
                prevViewZ,
                currRough);

            float score = guideScore * wViewZ;

            if (score > 0.10f)
            {
                float histConf = saturate(score);

                if (ViewZValid(outViewZ))
                {
                    // Alpha is the current-sample weight. Low alpha gives stable history;
                    // high alpha follows current raw guide more aggressively.
                    float currentWeight = saturate(Alpha);
                    outViewZ = lerp(prevViewZ, outViewZ, currentWeight);
                    outConf = max(outConf, histConf);
                }
                else
                {
                    outViewZ = prevViewZ;
                    outConf = max(outConf, histConf * 0.85f);
                }
            }
        }
    }

    // Current-frame hole fill from raw neighboring ViewZ samples.
    if (centerSurfaceValid &&
    (!ViewZValid(outViewZ) || outConf < 0.35f) &&
    currDepth < 0.9999f)
    {
        float bestScore = 0.0f;
        float bestViewZ = VIEWZ_INVALID;

        const int r = int(Radius);

        for (int y = -r; y <= r; ++y)
        {
            for (int x = -r; x <= r; ++x)
            {
                if (x == 0 && y == 0)
                    continue;

                int2 q = int2(pixel) + int2(x, y);
                q = clamp(q, int2(0, 0), int2(int(width) - 1, int(height) - 1));

                uint qSurfaceId = g_SurfaceId[q];

                if (qSurfaceId != centerSurfaceId)
                    continue;

                float candidateViewZ = g_RawViewZ[q];

                if (!ViewZValid(candidateViewZ))
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
                float spatialW = exp(
                -spatialDist2 /
                max(1e-5f, 2.0f * sigmaSpatial * sigmaSpatial));

                float wViewZ = ViewZSimilarityWeight(
                outViewZ,
                candidateViewZ,
                currRough);

                float score = guideScore * spatialW * wViewZ;

                if (score > bestScore)
                {
                    bestScore = score;
                    bestViewZ = candidateViewZ;
                }
            }
        }

        if (bestScore > 0.05f && ViewZValid(bestViewZ))
        {
            if (ViewZValid(outViewZ))
            {
                float fillW = saturate((1.0f - outConf) * bestScore);
                outViewZ = lerp(outViewZ, bestViewZ, fillW);
                outConf = max(outConf, bestScore * 0.65f);
            }
            else
            {
                outViewZ = bestViewZ;
                outConf = bestScore * 0.65f;
            }
        }
    }

    if (!centerSurfaceValid || currDepth >= 0.9999f)
    {
        outViewZ = VIEWZ_INVALID;
        outConf = 0.0f;
    }

    g_ViewZRecons[pixel] = ViewZValid(outViewZ) ? outViewZ : VIEWZ_INVALID;
    g_ViewZReconsConf[pixel] = saturate(outConf);

    WriteDebug(pixel, outViewZ, outConf, currRough);
}

#include "Common.hlsli"

// 81 = diffuse outlier clamp factor
// 82 = specular outlier clamp factor
// 83 = invalid/NaN/Inf sanitized mask
// 86 = outlier neighborhood valid-weight heatmap

Texture2D<float4> g_Input : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<uint> g_SurfaceId : register(t3);
Texture2D<float> g_ViewZ : register(t4);
Texture2D<float> g_ViewZConf : register(t5);
Texture2D<float> g_MotionConf : register(t6);

RWTexture2D<float4> g_Output : register(u0);
RWTexture2D<float4> g_Debug : register(u1);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

cbuffer RtOutlierClampConstants : register(b0)
{
    float2 InvResolution;
    uint Radius;
    uint SignalKind; // 0 diffuse, 1 spec

    float DepthSigma;
    float NormalSigma;
    float RoughnessSigma;
    float _padGuide0;

    float SigmaK;
    float MaxLuminance;
    float MinNeighborhoodWeight;
    float MinClampLuminance;

    float SurfaceIdRequired;
    float ClampStrength;
    float MotionRelaxation;
    uint DebugView;

    float3 DistanceNormParams;
    float DistanceNormSigma;

    uint UseViewZ;
    uint UseMotionConf;
    uint2 _pad0;
};

float3 DecodeNormal(float4 packed)
{
    return SafeNormalize(packed.xyz * 2.0f - 1.0f);
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float SurfaceWeight(uint a, uint b)
{
    if (SurfaceIdRequired < 0.5f)
        return 1.0f;

    if (!SurfaceIdValid(a) || !SurfaceIdValid(b))
        return 0.0f;

    return a == b ? 1.0f : 0.0f;
}

struct NeighborhoodStats
{
    float weight;
    float mean;
    float meanSq;
};

float ViewZWeight(uint2 p0, uint2 p1, float roughness)
{
    if (UseViewZ == 0u)
        return 1.0f;

    float conf0 = saturate(g_ViewZConf[p0]);
    float conf1 = saturate(g_ViewZConf[p1]);

    if (conf0 < 0.5f || conf1 < 0.5f)
        return 1.0f;

    float z0 = g_ViewZ[p0];
    float z1 = g_ViewZ[p1];

    if (!DistanceValid(z0) || !DistanceValid(z1))
        return 1.0f;

    float n0 = NormalizeDistance(z0, z0, roughness, DistanceNormParams);
    float n1 = NormalizeDistance(z1, z0, roughness, DistanceNormParams);

    return DistanceSimilarityWeight(n0, n1, DistanceNormSigma);
}

NeighborhoodStats BuildStats(uint2 pixel, uint width, uint height)
{
    NeighborhoodStats s;
    s.weight = 0.0f;
    s.mean = 0.0f;
    s.meanSq = 0.0f;

    float4 centerNR = g_Normal[pixel];
    float3 centerN = DecodeNormal(centerNR);
    float centerRough = centerNR.a;
    float centerDepth = g_Depth[pixel];
    uint centerId = g_SurfaceId[pixel];

    if (!SurfaceIdValid(centerId) || centerDepth >= 0.9999f)
        return s;

    const int r = int(Radius);

    for (int y = -r; y <= r; ++y)
    {
        for (int x = -r; x <= r; ++x)
        {
            if (x == 0 && y == 0)
                continue;

            int2 qI = int2(pixel) + int2(x, y);
            qI = clamp(qI, int2(0, 0), int2(int(width) - 1, int(height) - 1));
            uint2 q = uint2(qI);

            uint qId = g_SurfaceId[q];
            float wSurface = SurfaceWeight(centerId, qId);
            if (wSurface <= 0.0f)
                continue;

            float qDepth = g_Depth[q];
            if (qDepth >= 0.9999f)
                continue;

            float4 qNR = g_Normal[q];
            float3 qN = DecodeNormal(qNR);
            float qRough = qNR.a;

            float wDepth = exp(-abs(centerDepth - qDepth) / max(1e-5f, DepthSigma));
            float wNormal = exp(-(1.0f - saturate(dot(centerN, qN))) / max(1e-5f, NormalSigma));
            float wRough = exp(-abs(centerRough - qRough) / max(1e-5f, RoughnessSigma));
            float wZ = ViewZWeight(pixel, q, centerRough);

            float lum = SafeLuminance(g_Input[q].rgb);
            float w = wSurface * wDepth * wNormal * wRough * wZ;

            s.mean += lum * w;
            s.meanSq += lum * lum * w;
            s.weight += w;
        }
    }

    if (s.weight > 1e-6f)
    {
        s.mean /= s.weight;
        s.meanSq /= s.weight;
    }

    return s;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 raw = g_Input[pixel].rgb;
    bool invalidInput = !IsFinite3(raw) || any(raw < 0.0f.xxx);

    float3 center = SanitizeRadiance(raw);
    float centerLum = SafeLuminance(center);

    NeighborhoodStats stats = BuildStats(pixel, width, height);

    float motionConf = (UseMotionConf != 0u) ? saturate(g_MotionConf[pixel]) : 1.0f;
    float sigmaK = SigmaK * lerp(1.0f + max(0.0f, MotionRelaxation), 1.0f, motionConf);

    float clampLum = min(centerLum, MaxLuminance);
    float clampFactor = centerLum > RT_LUMINANCE_EPS
        ? clampLum / centerLum
        : 1.0f;

    if (stats.weight >= MinNeighborhoodWeight)
    {
        float sigma = RobustSigmaFromMoments(stats.mean, stats.meanSq);
        float upper = min(MaxLuminance, max(MinClampLuminance, stats.mean + sigmaK * sigma));

        if (centerLum > upper)
        {
            float proposedLum =
                lerp(centerLum, upper, saturate(ClampStrength));

            clampLum =
                min(MaxLuminance, max(MinClampLuminance, proposedLum));

            clampFactor =
                clampLum / max(centerLum, RT_LUMINANCE_EPS);
        }
    }

    float3 outRgb = ScaleToLuminance(center, clampLum);
    float alpha = IsFiniteScalar(g_Input[pixel].a)
        ? max(0.0f, g_Input[pixel].a)
        : 0.0f;

    g_Output[pixel] = float4(outRgb, alpha);

    if (DebugView == 81u && SignalKind == 0u)
    {
        g_Debug[pixel] = float4(saturate(clampFactor).xxx, 1.0f);
    }
    else if (DebugView == 82u && SignalKind == 1u)
    {
        g_Debug[pixel] = float4(saturate(clampFactor).xxx, 1.0f);
    }
    else if (DebugView == 83u)
    {
        g_Debug[pixel] = float4((invalidInput ? 1.0f : 0.0f).xxx, 1.0f);
    }
    else if (DebugView == 86u)
    {
        float v = saturate(stats.weight / max(1e-4f, MinNeighborhoodWeight * 4.0f));
        g_Debug[pixel] = float4(v.xxx, 1.0f);
    }
}

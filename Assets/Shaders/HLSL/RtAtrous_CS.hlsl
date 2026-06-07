#include "Common.hlsli"

// RT DebugView ownership for this pass:
// SVGF / A-Trous owns:
//   28 = roughness/specular protection proxy
//   43 = center-history length attenuation factor
//   44 = wide-iteration skip mask
//   60 = spec A-Trous shaped motion confidence
//   73 = spec A-Trous ViewZ shaped weight
//   78 = surface-id edge stop factor

Texture2D<float4> g_Signal : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float2> g_Moments : register(t3);
Texture2D<float> g_MotionConf : register(t4);
Texture2D<float> g_ViewZ : register(t5);
Texture2D<float> g_ViewZConf : register(t6);
Texture2D<uint> g_SurfaceId : register(t7);

RWTexture2D<float4> g_Output : register(u0);

SamplerState g_LinearClamp : register(s0);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

cbuffer RtAtrousConstants : register(b0)
{
    float2 InvResolution;
    uint IterationIndex;
    uint StepWidth;

    float SigmaDepth;
    float SigmaNormal;
    float VarianceScale;
    uint UseMoments;

    float LengthAttenuation;
    float LengthPower;
    uint FinalOutputSrgb;
    uint DebugView;
    
    float LengthSkipThreshold;
    uint EnableLengthSkip;
    float MotionConfPower;
    float MotionConfMin;
    
    float ViewZSigmaScale;
    float ViewZRoughCutoff;

    float ViewZConfMin;
    float _padViewZ0;

    float3 DistanceNormParams;
    float DistanceNormSigma;
    
    float AtrousContributionMaxLuminance;
    float3 _padSafety0;
};

float3 UnpackNormal(float4 packed)
{
    return SafeNormalize(packed.xyz * 2.0f - 1.0f);
}

float Luminance(float3 c)
{
    return SafeLuminance(c);
}

float EvalViewZAtrousWeight(
    float viewZ0,
    float viewZ1,
    float conf0,
    float conf1,
    float roughMin)
{
    // MotionConfPower > 0 remains the existing spec-path indicator.
    if (MotionConfPower <= 0.0f || ViewZSigmaScale <= 0.0f)
        return 1.0f;

    if (roughMin >= ViewZRoughCutoff)
        return 1.0f;

    if (!DistanceValid(viewZ0) || !DistanceValid(viewZ1))
        return 1.0f;

    if (conf0 < ViewZConfMin || conf1 < ViewZConfMin)
        return 1.0f;

    float n0 = NormalizeDistance(
        viewZ0,
        viewZ0,
        roughMin,
        DistanceNormParams);

    float n1 = NormalizeDistance(
        viewZ1,
        viewZ0,
        roughMin,
        DistanceNormParams);

    float sigma = max(
        1e-4f,
        DistanceNormSigma * max(0.0f, ViewZSigmaScale));

    return DistanceSimilarityWeight(n0, n1, sigma);
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float SurfaceIdEdgeWeight(uint id0, uint id1)
{
    if (!SurfaceIdValid(id0) || !SurfaceIdValid(id1))
        return 0.0f;

    return (id0 == id1) ? 1.0f : 0.0f;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    static const float kernel[3] = { 1.0f, 2.0f, 1.0f };

    float4 centerSig = g_Signal[pixel];

    float3 c0 = ClampRadianceLuminance(
        centerSig.rgb,
        AtrousContributionMaxLuminance);

    float len0 = IsFiniteScalar(centerSig.a)
        ? clamp(centerSig.a, 0.0f, 255.0f)
        : 0.0f;
    float lenN0 = saturate(len0 / 255.0f);

    float motionConfRaw = saturate(g_MotionConf[pixel]);

    // MotionConfPower <= 0 is the no-op path used by diffuse A-Trous.
    // Spec A-Trous passes the spec temporal confidence policy here.
    float motionConfW = 1.0f;
    if (MotionConfPower > 0.0f)
    {
        motionConfW = pow(motionConfRaw, max(MotionConfPower, 1e-3f));

        // Below the spec temporal confidence threshold, treat the history as
        // untrusted for center-protection purposes.
        if (motionConfRaw < saturate(MotionConfMin))
        {
            motionConfW = 0.0f;
        }
    }

    float lenEffective = lenN0 * motionConfW;
    float lenCurve = pow(lenEffective, max(1e-4f, LengthPower));
    float wLenCenter = lerp(1.0f, LengthAttenuation, lenCurve);

    bool skipWide =
    (EnableLengthSkip != 0) &&
    (IterationIndex > 0) &&
    (lenEffective >= LengthSkipThreshold);
    
    float4 n0Packed = g_Normal[pixel];
    float3 n0 = UnpackNormal(n0Packed);
    float rough0 = n0Packed.a;
    float z0 = g_Depth[pixel];
    
    float viewZ0 =
    (ViewZSigmaScale > 0.0f)
        ? g_ViewZ[pixel]
        : DISTANCE_INVALID;

    float viewZConf0 =
    (ViewZSigmaScale > 0.0f)
        ? saturate(g_ViewZConf[pixel])
        : 0.0f;
    
    uint surfaceId0 = g_SurfaceId[pixel];
    
    float l0 = 0.0f;
    float sigmaL = 1.0f;

    if (UseMoments != 0)
    {
        float2 m0 = g_Moments[pixel];

        float m1 = IsFiniteScalar(m0.x) ? max(m0.x, 0.0f) : SafeLuminance(c0);
        float m2 = IsFiniteScalar(m0.y) ? m0.y : (m1 * m1);

        m2 = max(m2, m1 * m1);

        float variance = max(m2 - m1 * m1, 0.0f);
        variance = min(
            variance,
            AtrousContributionMaxLuminance * AtrousContributionMaxLuminance);

        sigmaL = max(1e-4f, VarianceScale * sqrt(variance));
        l0 = SafeLuminance(c0);
    }
    
    if (DebugView == 28)
    {
        float protectionProxy = lerp(128.0f, 32.0f, rough0) / 128.0f;
        g_Output[pixel] = float4(protectionProxy.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 43)
    {
        g_Output[pixel] = float4(wLenCenter.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 44)
    {
        g_Output[pixel] = float4(skipWide ? 1.0f.xxx : 0.0f.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 60)
    {
        g_Output[pixel] = float4(motionConfW.xxx, 1.0f);
        return;
    }
    
    if (skipWide && DebugView != 73 && DebugView != 78)
    {
        if (FinalOutputSrgb != 0)
            g_Output[pixel] = float4(LinearToSRGB(c0), len0);
        else
            g_Output[pixel] = float4(c0, len0);

        return;
    }
    
    float3 sum = 0.0f.xxx;
    float wsum = 0.0f;
    
    float viewZWeightDebugSum = 0.0f;
    float viewZWeightDebugWeight = 0.0f;
    float surfaceIdDebugSum = 0.0f;
    float surfaceIdDebugWeight = 0.0f;
    
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            int2 offset = int2(x, y) * int(StepWidth);
            int2 p = int2(pixel) + offset;
            p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float4 sig = g_Signal[uint2(p)];

            float3 c = ClampRadianceLuminance(
                sig.rgb,
                AtrousContributionMaxLuminance);

            float4 nPacked = g_Normal[uint2(p)];
            float3 n = UnpackNormal(nPacked);
            float rough = nPacked.a;
            float z = g_Depth[uint2(p)];

            float ws = kernel[abs(x)] * kernel[abs(y)];

            float roughMin = min(rough0, rough);
            float viewZ1 = DISTANCE_INVALID;
            float viewZConf1 = 0.0f;

            if (ViewZSigmaScale > 0.0f)
            {
                viewZ1 = g_ViewZ[uint2(p)];
                viewZConf1 = saturate(g_ViewZConf[uint2(p)]);
            }

            float wViewZ = EvalViewZAtrousWeight(
                viewZ0,
                viewZ1,
                viewZConf0,
                viewZConf1,
                roughMin);
            
            float basePow = lerp(128.0f, 32.0f, roughMin);
            float normalScale = clamp(0.25f / max(1e-4f, SigmaNormal), 0.5f, 2.0f);
            float normalPow = basePow * normalScale;
            float wn = pow(saturate(dot(n0, n)), normalPow);

            float wz = exp(-abs(z0 - z) / max(1e-4f, SigmaDepth));

            float wl = 1.0f;
            if (UseMoments != 0)
            {
                float l = SafeLuminance(c);
                float sigmaLUsed = max(1e-4f, sigmaL * lerp(0.5f, 1.0f, roughMin));
                wl = exp(-abs(l - l0) / sigmaLUsed);

            }
            bool isCenterTap = (offset.x == 0 && offset.y == 0);

            float wLen = isCenterTap ? 1.0f : wLenCenter;
            
            uint surfaceId1 = g_SurfaceId[uint2(p)];
            float wSurfaceId = SurfaceIdEdgeWeight(surfaceId0, surfaceId1);

            float w = ws * wn * wz * wl * wLen * wViewZ * wSurfaceId;
            
            if (!IsFiniteScalar(w) || w <= 0.0f)
                continue;
            
            sum += c * w;
            wsum += w;
            viewZWeightDebugSum += wViewZ * ws;
            viewZWeightDebugWeight += ws;
            surfaceIdDebugSum += wSurfaceId * ws;
            surfaceIdDebugWeight += ws;
        }
    }

    if (DebugView == 73)
    {
        float v = (viewZWeightDebugWeight > 1e-6f)
        ? saturate(viewZWeightDebugSum / viewZWeightDebugWeight)
        : 1.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 78)
    {
        float v = (surfaceIdDebugWeight > 1e-6f)
        ? saturate(surfaceIdDebugSum / surfaceIdDebugWeight)
        : 0.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }
    
    float3 filtered = (wsum > 1e-6f) ? (sum / wsum) : c0;

    filtered = ClampRadianceLuminance(
        filtered,
        AtrousContributionMaxLuminance);

    if (FinalOutputSrgb != 0)
        g_Output[pixel] = float4(LinearToSRGB(filtered), len0);
    else
        g_Output[pixel] = float4(filtered, len0);
}
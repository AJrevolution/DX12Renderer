#include "Common.hlsli"
#include "RtReservoir.hlsli"

// RT DebugView ownership for this pass:
//   106 = previous-frame reservoir reuse accepted mask
//   107 = R: normalized M, G: normalized age, B: reuse confidence

StructuredBuffer<RtRestirReservoir> g_CurrInitialReservoir : register(t0);
StructuredBuffer<RtRestirReservoir> g_PrevTemporalReservoir : register(t1);

Texture2D<float4> g_CurrNormal : register(t2);
Texture2D<float> g_CurrDepth : register(t3);
Texture2D<float2> g_CurrPrevUV : register(t4);
Texture2D<float> g_CurrViewZ : register(t5);
Texture2D<uint> g_CurrSurfaceId : register(t6);

Texture2D<float4> g_PrevNormal : register(t7);
Texture2D<float> g_PrevDepth : register(t8);
Texture2D<float> g_PrevViewZ : register(t9);
Texture2D<uint> g_PrevSurfaceId : register(t10);

RWStructuredBuffer<RtRestirReservoir> g_OutTemporalReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

cbuffer RtRestirTemporalConstants : register(b0)
{
    float2 InvResolution;
    uint TemporalEnabled;
    uint HistoryValid;

    uint SurfaceIdHistoryValid;
    uint ViewZHistoryValid;
    uint DebugView;
    uint FrameIndex;

    float DepthSigma;
    float NormalSigma;
    float RoughnessSigma;
    float ViewZSigmaScale;

    float ReprojectMinWeight;
    float MaxM;
    float MaxAge;
    float MaxWeight;

    float3 DistanceNormParams;
    float DistanceNormSigma;

    uint MathMode;
    uint3 _padMath;
};

uint HashUintRtRestir(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float3 UnpackNormal(float4 packed)
{
    return SafeNormalize(packed.xyz * 2.0f - 1.0f);
}

bool PrevUVValid(float2 uv)
{
    // The upper edge is exclusive. uv == 1 maps one texel beyond the resource
    // and is a disocclusion, not a valid reprojection that should be clamped.
    return
        all(uv >= 0.0f.xx) &&
        all(uv < 1.0f.xx) &&
        !any(isnan(uv)) &&
        !any(isinf(uv));
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

bool CurrentGuideValid(uint2 pixel)
{
    const uint surfaceId = g_CurrSurfaceId[pixel];
    const float depth = g_CurrDepth[pixel];
    const float viewZ = g_CurrViewZ[pixel];

    return
        SurfaceIdValid(surfaceId) &&
        depth < 0.9999f &&
        DistanceValid(viewZ);
}

bool EvaluateTemporalGuideWeight(
    uint2 currPixel,
    uint2 prevPixel,
    out float reuseWeight)
{
    reuseWeight = 0.0f;

    if (SurfaceIdHistoryValid == 0u || ViewZHistoryValid == 0u)
        return false;

    const uint currId = g_CurrSurfaceId[currPixel];
    const uint prevId = g_PrevSurfaceId[prevPixel];

    if (!SurfaceIdValid(currId) ||
        !SurfaceIdValid(prevId) ||
        currId != prevId)
    {
        return false;
    }

    const float currDepth = g_CurrDepth[currPixel];
    const float prevDepth = g_PrevDepth[prevPixel];

    if (currDepth >= 0.9999f || prevDepth >= 0.9999f)
        return false;

    const float currViewZ = g_CurrViewZ[currPixel];
    const float prevViewZ = g_PrevViewZ[prevPixel];

    if (!DistanceValid(currViewZ) || !DistanceValid(prevViewZ))
        return false;

    const float4 currNR = g_CurrNormal[currPixel];
    const float4 prevNR = g_PrevNormal[prevPixel];
    const float3 currNormal = UnpackNormal(currNR);
    const float3 prevNormal = UnpackNormal(prevNR);
    const float normalDot = saturate(dot(currNormal, prevNormal));

    // SurfaceId is a hard identity gate. The remaining guide tests protect
    // against animation, disocclusion, and large geometric changes within an ID.
    if (normalDot < 0.95f)
        return false;

    const float currRoughness = saturate(currNR.a);
    const float prevRoughness = saturate(prevNR.a);

    const float normalWeight =
        exp(-(1.0f - normalDot) / max(1.0e-5f, NormalSigma));

    const float depthWeight =
        exp(-abs(currDepth - prevDepth) / max(1.0e-5f, DepthSigma));

    const float roughnessWeight =
        exp(-abs(currRoughness - prevRoughness) /
            max(1.0e-5f, RoughnessSigma));

    // Use the same normalized-distance contract as the ViewZ reconstruction,
    // temporal denoiser, and spatial ReSTIR pass. Raw metre differences are not
    // stable across scene scale and distance.
    const float currNormZ =
        NormalizeDistance(
            currViewZ,
            currViewZ,
            currRoughness,
            DistanceNormParams);

    const float prevNormZ =
        NormalizeDistance(
            prevViewZ,
            currViewZ,
            currRoughness,
            DistanceNormParams);

    const float viewZSigma =
        max(1.0e-5f, DistanceNormSigma * max(1.0e-5f, ViewZSigmaScale));

    const float viewZWeight =
        DistanceSimilarityWeight(currNormZ, prevNormZ, viewZSigma);

    reuseWeight =
        normalWeight *
        depthWeight *
        roughnessWeight *
        viewZWeight;

    return
        RtReservoirFiniteScalar(reuseWeight) &&
        reuseWeight > 0.0f;
}

bool RetargetPreviousReservoir(
    inout RtRestirReservoir previous,
    uint2 currPixel,
    uint2 prevPixel,
    out float currentTarget)
{
    currentTarget = 0.0f;

    if (!ReservoirFinalizedValid(previous))
        return false;

    const float3 currNormal =
        UnpackNormal(g_CurrNormal[currPixel]);

    const float3 prevNormal =
        UnpackNormal(g_PrevNormal[prevPixel]);

    const float3 wi = SafeNormalize(previous.sampleDir_pdf.xyz);
    const float currNoL = saturate(dot(currNormal, wi));
    const float prevNoL = saturate(dot(prevNormal, wi));

    if (currNoL <= 1.0e-4f || prevNoL <= 1.0e-4f)
        return false;

    const float cosineRatio = currNoL / prevNoL;

    // The hard normal/roughness/SurfaceId gates make this receiver-domain
    // retarget conservative. SurfaceId includes object/material identity, while
    // this ratio corrects the selected sample's dominant geometric target term.
    if (!RtReservoirFiniteScalar(cosineRatio) ||
        cosineRatio < 0.5f ||
        cosineRatio > 2.0f)
    {
        return false;
    }

    currentTarget = ReservoirTarget(previous) * cosineRatio;
    return ReservoirRetarget(previous, currentTarget);
}

float3 Heat(float v)
{
    v = saturate(v);

    return saturate(float3(
        smoothstep(0.35f, 1.00f, v),
        smoothstep(0.10f, 0.80f, v) * (1.0f - smoothstep(0.85f, 1.00f, v)),
        1.0f - smoothstep(0.00f, 0.65f, v)));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;

    uint width;
    uint height;
    g_Output.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    const uint pixelIndex = pixel.y * width + pixel.x;
    const RtRestirReservoir current =
        g_CurrInitialReservoir[pixelIndex];

    RtRestirReservoir outputReservoir;
    ReservoirClear(outputReservoir);

    uint rng =
        HashUintRtRestir(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0xB5297A4Du);

    bool usedPrevious = false;

    if (ReservoirFinalizedValid(current))
    {
        const float currentM =
            max(1.0f, min(current.weightSum_M_W.y, MaxM));

        const float currentWeight =
            ReservoirReuseWeight(
                current,
                ReservoirTarget(current),
                currentM);

        ReservoirUpdateWeighted(
            outputReservoir,
            current,
            currentWeight,
            currentM,
            rng);
    }

    // A failed current environment candidate does not invalidate the visible
    // surface. Stable previous history may still provide a valid candidate.
    if (TemporalEnabled != 0u &&
        HistoryValid != 0u &&
        CurrentGuideValid(pixel))
    {
        const float2 prevUV = g_CurrPrevUV[pixel];

        if (PrevUVValid(prevUV))
        {
            const uint2 prevPixel =
                min(
                    uint2(prevUV * float2(width, height)),
                    uint2(width - 1u, height - 1u));

            const uint prevIndex =
                prevPixel.y * width + prevPixel.x;

            RtRestirReservoir previous =
                g_PrevTemporalReservoir[prevIndex];

            float guideWeight = 0.0f;
            float currentTarget = 0.0f;

            if (ReservoirFinalizedValid(previous) &&
                EvaluateTemporalGuideWeight(pixel, prevPixel, guideWeight) &&
                guideWeight >= ReprojectMinWeight &&
                float(previous.age) < MaxAge &&
                RetargetPreviousReservoir(
                    previous,
                    pixel,
                    prevPixel,
                    currentTarget))
            {
                previous.flags |= RT_RESTIR_RESERVOIR_REPROJECTED;
                previous.age = min(previous.age + 1u, 0xFFFFFFFEu);
                previous.weightSum_M_W.w = guideWeight;

                const float previousM =
                    max(1.0f, min(previous.weightSum_M_W.y, MaxM));

                // Guide confidence is a hard acceptance gate and debug/confidence signal.
                // It must not scale the mathematical RIS weight.
                const float previousWeight =
                    ReservoirReuseWeight(
                        previous,
                        currentTarget,
                        previousM);
                
                const RtReservoirUpdateResult updateResult =
                    ReservoirUpdateWeightedTracked(
                        outputReservoir,
                        previous,
                        previousWeight,
                        previousM,
                        rng);
                
                usedPrevious =
                    updateResult.accepted != 0u;
            }
        }
    }

    ReservoirFinalize(outputReservoir, MaxM, MaxWeight, MathMode);

    if (!ReservoirFinalizedValid(outputReservoir))
        ReservoirClear(outputReservoir);

    g_OutTemporalReservoir[pixelIndex] = outputReservoir;

    if (DebugView == 106u)
    {
        g_Output[pixel] =
            float4((usedPrevious ? 1.0f : 0.0f).xxx, 1.0f);
    }
    else if (DebugView == 107u)
    {
        const bool valid = ReservoirFinalizedValid(outputReservoir);
        const float normalizedM = valid
            ? saturate(outputReservoir.weightSum_M_W.y / max(1.0f, MaxM))
            : 0.0f;
        const float normalizedAge = valid && MaxAge > 0.0f
            ? saturate(float(outputReservoir.age) / MaxAge)
            : 0.0f;
        const float confidence = valid
            ? saturate(outputReservoir.weightSum_M_W.w)
            : 0.0f;

        g_Output[pixel] =
            float4(normalizedM, normalizedAge, confidence, 1.0f);
    }
}

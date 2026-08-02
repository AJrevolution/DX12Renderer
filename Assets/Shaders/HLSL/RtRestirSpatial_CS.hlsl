#include "Common.hlsli"
#include "RtReservoir.hlsli"

// RT DebugView ownership for this pass:
//   108 = accepted spatial reuse count
//   109 = selected neighbour distance

StructuredBuffer<RtRestirReservoir> g_TemporalReservoir : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<uint> g_CurrSurfaceId : register(t3);
Texture2D<float> g_CurrViewZ : register(t4);

RWStructuredBuffer<RtRestirReservoir> g_OutSpatialReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;
static const float RT_RESTIR_SPATIAL_PI = 3.14159265358979323846f;

cbuffer RtRestirSpatialConstants : register(b0)
{
    float2 InvResolution;
    uint SampleCount;
    uint Radius;

    float NormalSigma;
    float DepthSigma;
    float RoughnessSigma;
    float ViewZSigmaScale;

    float MaxM;
    float MaxWeight;
    uint FrameIndex;
    uint DebugView;

    float3 DistanceNormParams;
    float DistanceNormSigma;

    float SpatialMinReuseWeight;
    uint MathMode;
    uint2 _padMath;
};

uint HashUintRtSpatial(uint x)
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

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float3 Heat(float v)
{
    v = saturate(v);

    return saturate(float3(
        smoothstep(0.35f, 1.00f, v),
        smoothstep(0.10f, 0.80f, v) * (1.0f - smoothstep(0.85f, 1.00f, v)),
        1.0f - smoothstep(0.00f, 0.65f, v)));
}

bool LoadSpatialGuide(
    uint2 pixel,
    out float3 normal,
    out float roughness,
    out float depth,
    out uint surfaceId,
    out float viewZ)
{
    const float4 packedNormalRoughness = g_CurrNormal[pixel];

    normal = UnpackNormal(packedNormalRoughness);
    roughness = saturate(packedNormalRoughness.a);
    depth = g_CurrDepth[pixel];
    surfaceId = g_CurrSurfaceId[pixel];
    viewZ = g_CurrViewZ[pixel];

    return
        SurfaceIdValid(surfaceId) &&
        depth < 0.9999f &&
        DistanceValid(viewZ);
}

bool EvaluateSpatialGuideWeight(
    uint2 centerPixel,
    uint2 neighbourPixel,
    out float guideWeight)
{
    guideWeight = 0.0f;

    float3 centerNormal;
    float centerRoughness;
    float centerDepth;
    uint centerSurfaceId;
    float centerViewZ;

    if (!LoadSpatialGuide(
        centerPixel,
        centerNormal,
        centerRoughness,
        centerDepth,
        centerSurfaceId,
        centerViewZ))
    {
        return false;
    }

    float3 neighbourNormal;
    float neighbourRoughness;
    float neighbourDepth;
    uint neighbourSurfaceId;
    float neighbourViewZ;

    if (!LoadSpatialGuide(
        neighbourPixel,
        neighbourNormal,
        neighbourRoughness,
        neighbourDepth,
        neighbourSurfaceId,
        neighbourViewZ))
    {
        return false;
    }

    // SurfaceId is intentionally a hard gate. It prevents reuse across object
    // and material boundaries even when all continuous guides look similar.
    if (centerSurfaceId != neighbourSurfaceId)
        return false;

    const float normalDot =
        saturate(dot(centerNormal, neighbourNormal));

    const float normalWeight =
        exp(-(1.0f - normalDot) / max(1.0e-5f, NormalSigma));

    const float depthWeight =
        exp(-abs(centerDepth - neighbourDepth) /
            max(1.0e-5f, DepthSigma));

    const float roughnessWeight =
        exp(-abs(centerRoughness - neighbourRoughness) /
            max(1.0e-5f, RoughnessSigma));

    const float centerNormZ =
        NormalizeDistance(
            centerViewZ,
            centerViewZ,
            centerRoughness,
            DistanceNormParams);

    const float neighbourNormZ =
        NormalizeDistance(
            neighbourViewZ,
            centerViewZ,
            centerRoughness,
            DistanceNormParams);

    const float viewZSigma =
        max(1.0e-5f, DistanceNormSigma * max(1.0e-5f, ViewZSigmaScale));

    const float viewZWeight =
        DistanceSimilarityWeight(
            centerNormZ,
            neighbourNormZ,
            viewZSigma);

    if (normalWeight < 0.05f ||
        depthWeight < 0.05f ||
        roughnessWeight < 0.05f ||
        viewZWeight < 0.05f)
    {
        return false;
    }

    guideWeight =
        normalWeight *
        depthWeight *
        roughnessWeight *
        viewZWeight;

    return
        RtReservoirFiniteScalar(guideWeight) &&
        guideWeight > 0.0f;
}

bool RetargetNeighbourReservoir(
    inout RtRestirReservoir neighbour,
    uint2 centerPixel,
    uint2 neighbourPixel,
    out float centerTarget)
{
    centerTarget = 0.0f;

    if (!ReservoirFinalizedValid(neighbour))
        return false;

    const float3 centerNormal =
        UnpackNormal(g_CurrNormal[centerPixel]);

    const float3 neighbourNormal =
        UnpackNormal(g_CurrNormal[neighbourPixel]);

    const float3 wi = SafeNormalize(neighbour.sampleDir_pdf.xyz);
    const float centerNoL = saturate(dot(centerNormal, wi));
    const float neighbourNoL = saturate(dot(neighbourNormal, wi));

    if (centerNoL <= 1.0e-4f || neighbourNoL <= 1.0e-4f)
        return false;

    const float cosineRatio = centerNoL / neighbourNoL;

    if (!RtReservoirFiniteScalar(cosineRatio) ||
        cosineRatio < 0.5f ||
        cosineRatio > 2.0f)
    {
        return false;
    }

    centerTarget = ReservoirTarget(neighbour) * cosineRatio;
    return ReservoirRetarget(neighbour, centerTarget);
}

RtReservoirUpdateResult UpdateSpatialReservoir(
    inout RtRestirReservoir destination,
    RtRestirReservoir candidate,
    float candidateWeight,
    float candidateM,
    float candidateDistance,
    inout float selectedDistance,
    inout uint rng)
{
    const RtReservoirUpdateResult result =
        ReservoirUpdateWeightedTracked(
            destination,
            candidate,
            candidateWeight,
            candidateM,
            rng);

    if (result.selected != 0u)
    {
        selectedDistance =
            candidateDistance;
    }

    return result;
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

    RtRestirReservoir outputReservoir;
    ReservoirClear(outputReservoir);

    RtRestirReservoir center =
        g_TemporalReservoir[pixelIndex];

    uint rng =
        HashUintRtSpatial(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0x8DA6B343u);

    uint acceptedCount = 0u;
    float selectedDistance = 0.0f;

    if (ReservoirFinalizedValid(center))
    {
        const float centerM =
            max(1.0f, min(center.weightSum_M_W.y, MaxM));

        const float centerWeight =
            ReservoirReuseWeight(
                center,
                ReservoirTarget(center),
                centerM);

        UpdateSpatialReservoir(
            outputReservoir,
            center,
            centerWeight,
            centerM,
            0.0f,
            selectedDistance,
            rng);
    }

    const uint sampleCount = max(1u, SampleCount);
    const uint radius = max(1u, Radius);

    [loop]
    for (uint sampleIndex = 0u;
        sampleIndex < sampleCount;
        ++sampleIndex)
    {
        const float angle =
            RtReservoirRand01(rng) * 2.0f * RT_RESTIR_SPATIAL_PI;

        // sqrt produces uniform area density over the sampling disk.
        const float distance =
            sqrt(RtReservoirRand01(rng)) * float(radius);

        const int2 offset =
            int2(round(float2(cos(angle), sin(angle)) * distance));

        if (all(offset == 0))
            continue;

        const int2 neighbourPixelI = int2(pixel) + offset;

        if (neighbourPixelI.x < 0 ||
            neighbourPixelI.y < 0 ||
            neighbourPixelI.x >= int(width) ||
            neighbourPixelI.y >= int(height))
        {
            continue;
        }

        const uint2 neighbourPixel = uint2(neighbourPixelI);
        const uint neighbourIndex =
            neighbourPixel.y * width + neighbourPixel.x;

        RtRestirReservoir neighbour =
            g_TemporalReservoir[neighbourIndex];

        float guideWeight = 0.0f;
        float centerTarget = 0.0f;

        if (!ReservoirFinalizedValid(neighbour) ||
            !EvaluateSpatialGuideWeight(
                pixel,
                neighbourPixel,
                guideWeight) ||
            !RetargetNeighbourReservoir(
                neighbour,
                pixel,
                neighbourPixel,
                centerTarget))
        {
            continue;
        }

        // Confidence controls eligibility. It does not scale the represented energy.
        if (guideWeight < SpatialMinReuseWeight)
        {
            continue;
        }

        neighbour.flags |=
            RT_RESTIR_RESERVOIR_SPATIAL;

        neighbour.weightSum_M_W.w =
            guideWeight;

        const float neighbourM =
        max(1.0f, min(neighbour.weightSum_M_W.y, MaxM));

        const float neighbourWeight =
            ReservoirReuseWeight(
                neighbour,
                centerTarget,
                neighbourM);

        const RtReservoirUpdateResult updateResult =
            UpdateSpatialReservoir(
                outputReservoir,
                neighbour,
                neighbourWeight,
                neighbourM,
                length(float2(offset)),
                selectedDistance,
                rng);

        if (updateResult.accepted != 0u)
        {
            acceptedCount++;
        }
    }

    ReservoirFinalize(outputReservoir, MaxM, MaxWeight, MathMode);

    if (!ReservoirFinalizedValid(outputReservoir))
        ReservoirClear(outputReservoir);

    g_OutSpatialReservoir[pixelIndex] = outputReservoir;

    if (DebugView == 108u)
    {
        const float value =
            float(acceptedCount) / float(sampleCount);

        g_Output[pixel] = float4(Heat(value), 1.0f);
    }
    else if (DebugView == 109u)
    {
        if (!ReservoirFinalizedValid(outputReservoir))
        {
            g_Output[pixel] = float4(0.0f.xxx, 1.0f);
            return;
        }

        const float value =
            saturate(selectedDistance / float(radius));

        g_Output[pixel] = float4(Heat(value), 1.0f);
    }
}

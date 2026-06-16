#include "Common.hlsli"
#include "RtReservoir.hlsli"

// RT DebugView ownership for this pass:
//   108 = accepted spatial reuse count
//   109 = selected neighbor distance

StructuredBuffer<RtRestirReservoir> g_TemporalReservoir : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<uint> g_CurrSurfaceId : register(t3);
Texture2D<float> g_CurrViewZ : register(t4);

RWStructuredBuffer<RtRestirReservoir> g_OutSpatialReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;
static const float kPiRestirSpatial = 3.14159265f;

cbuffer RtRestirSpatialConstants : register(b0)
{
    float2 InvResolution;
    uint SampleCount;
    uint Radius;

    float NormalSigma;
    float DepthSigma;
    float RoughnessSigma;
    float ViewZSigma;

    float MaxM;
    float MaxWeight;
    uint FrameIndex;
    uint DebugView;

    float3 DistanceNormParams;
    float DistanceNormSigma;
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
    float4 nr = g_CurrNormal[pixel];

    normal = UnpackNormal(nr);
    roughness = nr.a;
    depth = g_CurrDepth[pixel];
    surfaceId = g_CurrSurfaceId[pixel];
    viewZ = g_CurrViewZ[pixel];

    if (!SurfaceIdValid(surfaceId))
        return false;

    if (depth >= 0.9999f)
        return false;

    if (!DistanceValid(viewZ))
        return false;

    return true;
}

bool SpatialGuideAccept(
    uint2 centerPixel,
    uint2 neighborPixel,
    out float guideWeight)
{
    guideWeight = 0.0f;

    float3 centerN;
    float centerR;
    float centerDepth;
    uint centerSurfaceId;
    float centerViewZ;

    if (!LoadSpatialGuide(
        centerPixel,
        centerN,
        centerR,
        centerDepth,
        centerSurfaceId,
        centerViewZ))
    {
        return false;
    }

    float3 neighborN;
    float neighborR;
    float neighborDepth;
    uint neighborSurfaceId;
    float neighborViewZ;

    if (!LoadSpatialGuide(
        neighborPixel,
        neighborN,
        neighborR,
        neighborDepth,
        neighborSurfaceId,
        neighborViewZ))
    {
        return false;
    }

    if (centerSurfaceId != neighborSurfaceId)
        return false;

    float nd = saturate(dot(centerN, neighborN));

    float wNormal =
        exp(-(1.0f - nd) / max(1e-5f, NormalSigma));

    float wDepth =
        exp(-abs(centerDepth - neighborDepth) / max(1e-5f, DepthSigma));

    float wRough =
        exp(-abs(centerR - neighborR) / max(1e-5f, RoughnessSigma));

    float centerNormZ =
        NormalizeDistance(
            centerViewZ,
            centerViewZ,
            centerR,
            DistanceNormParams);

    float neighborNormZ =
        NormalizeDistance(
            neighborViewZ,
            centerViewZ,
            centerR,
            DistanceNormParams);

    float viewZSigma =
        max(1e-5f, DistanceNormSigma * max(1e-5f, ViewZSigma));

    float wViewZ =
        DistanceSimilarityWeight(
            centerNormZ,
            neighborNormZ,
            viewZSigma);

    // Treat ~3 sigma as the rejection boundary. This avoids accepting
    // completely unrelated pixels while still keeping the sigma knobs useful.
    if (wNormal < 0.05f ||
        wDepth < 0.05f ||
        wRough < 0.05f ||
        wViewZ < 0.05f)
    {
        return false;
    }

    guideWeight = wNormal * wDepth * wRough * wViewZ;
    return guideWeight > 0.0f;
}

void SpatialReservoirUpdate(
    inout RtRestirReservoir dst,
    RtRestirReservoir candidate,
    float candidateWeight,
    float candidateM,
    float candidateDistance,
    inout float selectedDistance,
    inout uint rng)
{
    if (!ReservoirValid(candidate))
        return;

    if (candidateWeight <= 0.0f || candidateM <= 0.0f)
        return;

    dst.weightSum_M_W.x += candidateWeight;
    dst.weightSum_M_W.y += candidateM;

    float p = candidateWeight / max(1e-8f, dst.weightSum_M_W.x);

    if (RtReservoirRand01(rng) < p)
    {
        dst.sampleDir_pdf = candidate.sampleDir_pdf;
        dst.sampleLi_target = candidate.sampleLi_target;
        dst.sampleIndex = candidate.sampleIndex;
        dst.surfaceId = candidate.surfaceId;
        dst.flags = candidate.flags | RT_RESTIR_RESERVOIR_VALID;
        dst.age = candidate.age;
        dst.weightSum_M_W.w = candidate.weightSum_M_W.w;

        selectedDistance = candidateDistance;
    }
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    uint pixelIndex = pixel.y * width + pixel.x;

    RtRestirReservoir outR;
    ReservoirClear(outR);

    RtRestirReservoir center = g_TemporalReservoir[pixelIndex];

    uint rng =
        HashUintRtSpatial(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0x8DA6B343u);

    uint acceptedCount = 0u;
    float selectedDistance = 0.0f;

    if (ReservoirValid(center))
    {
        center.weightSum_M_W.y =
            min(center.weightSum_M_W.y, MaxM);

        SpatialReservoirUpdate(
            outR,
            center,
            max(0.0f, center.weightSum_M_W.x),
            max(1.0f, center.weightSum_M_W.y),
            0.0f,
            selectedDistance,
            rng);
    }

    uint sampleCount = max(1u, SampleCount);
    uint radius = max(1u, Radius);

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        float angle = RtReservoirRand01(rng) * 2.0f * kPiRestirSpatial;
        float dist = sqrt(RtReservoirRand01(rng)) * float(radius);

        int2 offset =
            int2(round(float2(cos(angle), sin(angle)) * dist));

        if (offset.x == 0 && offset.y == 0)
            continue;

        int2 neighborI = int2(pixel) + offset;

        if (neighborI.x < 0 ||
            neighborI.y < 0 ||
            neighborI.x >= int(width) ||
            neighborI.y >= int(height))
        {
            continue;
        }

        uint2 neighborPixel = uint2(neighborI);
        uint neighborIndex = neighborPixel.y * width + neighborPixel.x;

        RtRestirReservoir neighbor = g_TemporalReservoir[neighborIndex];

        if (!ReservoirValid(neighbor))
            continue;

        float guideWeight = 0.0f;

        if (!SpatialGuideAccept(pixel, neighborPixel, guideWeight))
            continue;

        neighbor.flags |= RT_RESTIR_RESERVOIR_SPATIAL;
        neighbor.weightSum_M_W.y =
            min(neighbor.weightSum_M_W.y, MaxM);
        neighbor.weightSum_M_W.w =
            guideWeight;

        float candidateWeight =
            max(0.0f, neighbor.weightSum_M_W.x) *
            guideWeight;

        float candidateM =
            max(1.0f, neighbor.weightSum_M_W.y);

        SpatialReservoirUpdate(
            outR,
            neighbor,
            candidateWeight,
            candidateM,
            length(float2(offset)),
            selectedDistance,
            rng);

        acceptedCount++;
    }

    ReservoirFinalize(
        outR,
        MaxM,
        MaxWeight);

    if (!ReservoirValid(outR))
    {
        ReservoirClear(outR);
    }

    g_OutSpatialReservoir[pixelIndex] = outR;

    if (DebugView == 108u)
    {
        float v =
            sampleCount > 0u
                ? float(acceptedCount) / float(sampleCount)
                : 0.0f;

        g_Output[pixel] = float4(Heat(v), 1.0f);
    }
    else if (DebugView == 109u)
    {
        if (!ReservoirValid(outR))
        {
            g_Output[pixel] = float4(0.0f.xxx, 1.0f);
            return;
        }

        float v =
        radius > 0u
            ? saturate(selectedDistance / float(radius))
            : 0.0f;

        g_Output[pixel] = float4(Heat(v), 1.0f);
    }
}

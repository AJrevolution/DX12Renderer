#ifndef RT_RESERVOIR_HLSLI
#define RT_RESERVOIR_HLSLI

#include "Common.hlsli"

static const uint RT_RESTIR_RESERVOIR_VALID = 1u << 0;
static const uint RT_RESTIR_RESERVOIR_VISIBLE = 1u << 1;
static const uint RT_RESTIR_RESERVOIR_FALLBACK = 1u << 2;
static const uint RT_RESTIR_RESERVOIR_REPROJECTED = 1u << 3;
static const uint RT_RESTIR_RESERVOIR_SPATIAL = 1u << 4;

struct RtRestirReservoir
{
    float4 sampleDir_pdf; // xyz = wi, w = sourcePdf
    float4 sampleLi_target; // xyz = Li, w = targetLum
    float4 weightSum_M_W; // x = weightSum, y = M, z = W, w = confidence
    uint sampleIndex;
    uint flags;
    uint age;
    uint surfaceId;
};

uint RtReservoirLcg(inout uint s)
{
    s = 1664525u * s + 1013904223u;
    return s;
}

float RtReservoirRand01(inout uint s)
{
    return (RtReservoirLcg(s) & 0x00FFFFFFu) / 16777216.0f;
}

bool ReservoirSampleValid(RtRestirReservoir r)
{
    return
        (r.flags & RT_RESTIR_RESERVOIR_VALID) != 0u &&
        r.sampleDir_pdf.w > 1e-8f &&
        r.sampleLi_target.w > 1e-8f;
}

bool ReservoirValid(RtRestirReservoir r)
{
    return
        ReservoirSampleValid(r) &&
        r.weightSum_M_W.x >= 0.0f &&
        r.weightSum_M_W.y > 0.0f &&
        r.weightSum_M_W.z >= 0.0f;
}

void ReservoirClear(out RtRestirReservoir r)
{
    r.sampleDir_pdf = 0.0f.xxxx;
    r.sampleLi_target = 0.0f.xxxx;
    r.weightSum_M_W = 0.0f.xxxx;
    r.sampleIndex = 0u;
    r.flags = 0u;
    r.age = 0u;
    r.surfaceId = 0xFFFFFFFFu;
}

float ReservoirTarget(RtRestirReservoir r)
{
    return max(0.0f, r.sampleLi_target.w);
}

float ReservoirSourcePdf(RtRestirReservoir r)
{
    return max(0.0f, r.sampleDir_pdf.w);
}

float ReservoirCandidateWeight(float targetLum, float sourcePdf)
{
    if (targetLum <= 1e-8f || sourcePdf <= 1e-8f)
        return 0.0f;

    return targetLum / sourcePdf;
}

bool RtReservoirFiniteScalar(float v)
{
    return !isnan(v) && !isinf(v);
}

RtRestirReservoir MakeRestirCandidate(
    float3 wi,
    float3 Li,
    float sourcePdf,
    float targetLum,
    uint sampleIndex,
    uint surfaceId)
{
    RtRestirReservoir r;
    ReservoirClear(r);

    if (targetLum <= 1e-8f || sourcePdf <= 1e-8f)
        return r;

    r.sampleDir_pdf = float4(normalize(wi), sourcePdf);
    r.sampleLi_target = float4(max(Li, 0.0f.xxx), targetLum);
    // Raw candidate represents one candidate sample.
    // x = accumulated weight sum, unused for raw candidates
    // y = M, raw candidate count
    // z = finalized W, not finalized yet
    // w = confidence
    r.weightSum_M_W = float4(0.0f, 1.0f, 0.0f, 1.0f);
    r.sampleIndex = sampleIndex;
    r.flags = RT_RESTIR_RESERVOIR_VALID;
    r.age = 0u;
    r.surfaceId = surfaceId;
    return r;
}

void ReservoirUpdateWeighted(
    inout RtRestirReservoir r,
    RtRestirReservoir candidate,
    float candidateWeight,
    float candidateM,
    inout uint rng)
{
    if (!ReservoirSampleValid(candidate))
        return;

    if (candidateWeight <= 0.0f || candidateM <= 0.0f)
        return;

    r.weightSum_M_W.x += candidateWeight;
    r.weightSum_M_W.y += candidateM;

    float p = candidateWeight / max(1e-8f, r.weightSum_M_W.x);

    if (RtReservoirRand01(rng) < p)
    {
        r.sampleDir_pdf = candidate.sampleDir_pdf;
        r.sampleLi_target = candidate.sampleLi_target;
        r.sampleIndex = candidate.sampleIndex;
        r.surfaceId = candidate.surfaceId;
        r.flags = candidate.flags | RT_RESTIR_RESERVOIR_VALID;
        r.age = candidate.age;
        r.weightSum_M_W.w = candidate.weightSum_M_W.w;
    }
}

void ReservoirUpdate(
    inout RtRestirReservoir r,
    RtRestirReservoir candidate,
    float candidateWeight,
    inout uint rng)
{
    ReservoirUpdateWeighted(r, candidate, candidateWeight, 1.0f, rng);
}

void ReservoirFinalize(inout RtRestirReservoir r, float maxM, float maxWeight)
{
    float target = ReservoirTarget(r);
    float M = min(maxM, r.weightSum_M_W.y);

    if (target <= 1e-8f || M <= 0.0f)
    {
        ReservoirClear(r);
        return;
    }

    r.weightSum_M_W.y = M;
    r.weightSum_M_W.z =
        min(maxWeight, r.weightSum_M_W.x / max(1e-8f, M * target));

    if (!RtReservoirFiniteScalar(r.weightSum_M_W.z) ||
        r.weightSum_M_W.z < 0.0f)
    {
        ReservoirClear(r);
        return;
    }

    r.flags |= RT_RESTIR_RESERVOIR_VALID;
}


#endif

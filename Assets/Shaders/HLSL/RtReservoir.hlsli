#ifndef RT_RESERVOIR_HLSLI
#define RT_RESERVOIR_HLSLI

#include "Common.hlsli"

static const uint RT_RESTIR_RESERVOIR_VALID = 1u << 0;
static const uint RT_RESTIR_RESERVOIR_VISIBLE = 1u << 1;
static const uint RT_RESTIR_RESERVOIR_FALLBACK = 1u << 2;
static const uint RT_RESTIR_RESERVOIR_REPROJECTED = 1u << 3;
static const uint RT_RESTIR_RESERVOIR_SPATIAL = 1u << 4;

static const uint RT_RESTIR_MATH_REFERENCE = 0u;
static const uint RT_RESTIR_MATH_ROBUST = 1u;

static const float RT_RESTIR_MIN_PDF = 1.0e-8f;
static const float RT_RESTIR_MIN_TARGET = 1.0e-8f;

struct RtRestirReservoir
{
    float4 sampleDir_pdf; // xyz = world-space wi, w = source solid-angle PDF
    float4 sampleLi_target; // xyz = lighting-environment Li, w = target luminance
    float4 weightSum_M_W; // x = weightSum, y = M, z = final W, w = confidence
    uint sampleIndex; // environment alias-table/debug index
    uint flags;
    uint age;
    uint surfaceId;
};

struct RtReservoirUpdateResult
{
    uint accepted;
    uint selected;
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

bool RtReservoirFiniteScalar(float v)
{
    return !isnan(v) && !isinf(v);
}

bool RtReservoirFiniteFloat3(float3 value)
{
    return
        RtReservoirFiniteScalar(value.x) &&
        RtReservoirFiniteScalar(value.y) &&
        RtReservoirFiniteScalar(value.z);
}

bool RtReservoirFiniteFloat4(float4 value)
{
    return
        RtReservoirFiniteScalar(value.x) &&
        RtReservoirFiniteScalar(value.y) &&
        RtReservoirFiniteScalar(value.z) &&
        RtReservoirFiniteScalar(value.w);
}

bool ReservoirSampleValid(RtRestirReservoir reservoir)
{
    const float directionLengthSq =
        dot(reservoir.sampleDir_pdf.xyz, reservoir.sampleDir_pdf.xyz);

    return
        (reservoir.flags & RT_RESTIR_RESERVOIR_VALID) != 0u &&
        RtReservoirFiniteFloat4(reservoir.sampleDir_pdf) &&
        RtReservoirFiniteFloat4(reservoir.sampleLi_target) &&
        directionLengthSq > RT_RESTIR_MIN_PDF &&
        reservoir.sampleDir_pdf.w > RT_RESTIR_MIN_PDF &&
        reservoir.sampleLi_target.w > RT_RESTIR_MIN_TARGET;
}

// Temporal history, spatial history, resolve, and debug inspection must only
// consume finalized reservoirs. Raw candidates have weightSum == 0 and W == 0.
bool ReservoirFinalizedValid(
    RtRestirReservoir reservoir)
{
    return
        ReservoirSampleValid(reservoir) &&
        RtReservoirFiniteFloat4(reservoir.weightSum_M_W) &&
        reservoir.weightSum_M_W.x > 0.0f &&
        reservoir.weightSum_M_W.y > 0.0f &&
        reservoir.weightSum_M_W.z > 0.0f &&
        reservoir.weightSum_M_W.w >= 0.0f;
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

float ReservoirTarget(RtRestirReservoir reservoir)
{
    return RtReservoirFiniteScalar(reservoir.sampleLi_target.w)
        ? max(0.0f, reservoir.sampleLi_target.w)
        : 0.0f;
}

float ReservoirSourcePdf(
    RtRestirReservoir reservoir)
{
    return RtReservoirFiniteScalar(
        reservoir.sampleDir_pdf.w)
        ? max(0.0f, reservoir.sampleDir_pdf.w)
        : 0.0f;
}

float ReservoirCandidateWeight(float targetLuminance, float sourcePdf)
{
    if (!RtReservoirFiniteScalar(targetLuminance) ||
        !RtReservoirFiniteScalar(sourcePdf) ||
        targetLuminance <= RT_RESTIR_MIN_TARGET ||
        sourcePdf <= RT_RESTIR_MIN_PDF)
    {
        return 0.0f;
    }

    const float weight = targetLuminance / sourcePdf;
    return RtReservoirFiniteScalar(weight) && weight > 0.0f
        ? weight
        : 0.0f;
}

// Converts a finalized source reservoir into a weighted candidate evaluated
// at the receiving surface.
//
// This is source.W * source.M * targetAtReceiver.
float ReservoirReuseWeight(
    RtRestirReservoir reservoir,
    float currentTarget,
    float candidateM)
{
    if (!ReservoirFinalizedValid(reservoir) ||
        !RtReservoirFiniteScalar(currentTarget) ||
        !RtReservoirFiniteScalar(candidateM) ||
        currentTarget <= RT_RESTIR_MIN_TARGET ||
        candidateM <= 0.0f)
    {
        return 0.0f;
    }

    const float weight =
        reservoir.weightSum_M_W.z *
        currentTarget *
        candidateM;

    return
        RtReservoirFiniteScalar(weight) &&
        weight > 0.0f
        ? weight
        : 0.0f;
}

bool ReservoirRetarget(
    inout RtRestirReservoir reservoir,
    float currentTarget)
{
    if (!ReservoirFinalizedValid(reservoir) ||
        !RtReservoirFiniteScalar(currentTarget) ||
        currentTarget <= RT_RESTIR_MIN_TARGET)
    {
        return false;
    }

    reservoir.sampleLi_target.w =
        currentTarget;

    return true;
}

RtRestirReservoir MakeRestirCandidate(
    float3 wi,
    float3 Li,
    float sourcePdf,
    float targetLuminance,
    uint sampleIndex,
    uint surfaceId)
{
    RtRestirReservoir reservoir;
    ReservoirClear(reservoir);

    const float directionLengthSq =
        dot(wi, wi);

    if (!RtReservoirFiniteFloat3(wi) ||
        !RtReservoirFiniteFloat3(Li) ||
        !RtReservoirFiniteScalar(sourcePdf) ||
        !RtReservoirFiniteScalar(targetLuminance) ||
        directionLengthSq <= RT_RESTIR_MIN_PDF ||
        sourcePdf <= RT_RESTIR_MIN_PDF ||
        targetLuminance <= RT_RESTIR_MIN_TARGET)
    {
        return reservoir;
    }

    reservoir.sampleDir_pdf =
        float4(
            wi * rsqrt(directionLengthSq),
            sourcePdf);

    reservoir.sampleLi_target =
        float4(
            max(Li, 0.0f.xxx),
            targetLuminance);

    // Raw candidate:
    //   M = 1
    //   weightSum = 0
    //   W = 0
    //
    // It is sample-valid but not finalized-valid.
    reservoir.weightSum_M_W =
        float4(
            0.0f,
            1.0f,
            0.0f,
            1.0f);

    reservoir.sampleIndex = sampleIndex;
    reservoir.flags = RT_RESTIR_RESERVOIR_VALID;
    reservoir.age = 0u;
    reservoir.surfaceId = surfaceId;

    return reservoir;
}

RtReservoirUpdateResult ReservoirUpdateWeightedTracked(
    inout RtRestirReservoir reservoir,
    RtRestirReservoir candidate,
    float candidateWeight,
    float candidateM,
    inout uint rng)
{
    RtReservoirUpdateResult result;
    result.accepted = 0u;
    result.selected = 0u;

    if (!ReservoirSampleValid(candidate) ||
        !RtReservoirFiniteScalar(candidateWeight) ||
        !RtReservoirFiniteScalar(candidateM) ||
        candidateWeight <= 0.0f ||
        candidateM <= 0.0f)
    {
        return result;
    }

    const float newWeightSum =
        reservoir.weightSum_M_W.x +
        candidateWeight;

    const float newM =
        reservoir.weightSum_M_W.y +
        candidateM;

    if (!RtReservoirFiniteScalar(newWeightSum) ||
        !RtReservoirFiniteScalar(newM) ||
        newWeightSum <= 0.0f ||
        newM <= 0.0f)
    {
        ReservoirClear(reservoir);
        return result;
    }

    reservoir.weightSum_M_W.x =
        newWeightSum;

    reservoir.weightSum_M_W.y =
        newM;

    result.accepted = 1u;

    const float selectionProbability =
        saturate(candidateWeight / newWeightSum);

    if (RtReservoirRand01(rng) <
        selectionProbability)
    {
        reservoir.sampleDir_pdf =
            candidate.sampleDir_pdf;

        reservoir.sampleLi_target =
            candidate.sampleLi_target;

        reservoir.sampleIndex =
            candidate.sampleIndex;

        reservoir.surfaceId =
            candidate.surfaceId;

        reservoir.flags =
            candidate.flags |
            RT_RESTIR_RESERVOIR_VALID;

        reservoir.age =
            candidate.age;

        reservoir.weightSum_M_W.w =
            RtReservoirFiniteScalar(
                candidate.weightSum_M_W.w)
            ? max(
                0.0f,
                candidate.weightSum_M_W.w)
            : 0.0f;

        result.selected = 1u;
    }

    return result;
}

void ReservoirUpdateWeighted(
    inout RtRestirReservoir reservoir,
    RtRestirReservoir candidate,
    float candidateWeight,
    float candidateM,
    inout uint rng)
{
    ReservoirUpdateWeightedTracked(
        reservoir,
        candidate,
        candidateWeight,
        candidateM,
        rng);
}


void ReservoirUpdate(
    inout RtRestirReservoir reservoir,
    RtRestirReservoir candidate,
    float candidateWeight,
    inout uint rng)
{
    ReservoirUpdateWeighted(
        reservoir,
        candidate,
        candidateWeight,
        1.0f,
        rng);
}

void ReservoirFinalize(
    inout RtRestirReservoir reservoir,
    float maxM,
    float maxWeight,
    uint mathMode)
{
    const float target =
        ReservoirTarget(reservoir);

    const float sourceM =
        reservoir.weightSum_M_W.y;

    const float safeMaxM =
        max(1.0f, maxM);

    if (!ReservoirSampleValid(reservoir) ||
        !RtReservoirFiniteScalar(sourceM) ||
        !RtReservoirFiniteScalar(
            reservoir.weightSum_M_W.x) ||
        target <= RT_RESTIR_MIN_TARGET ||
        sourceM <= 0.0f ||
        reservoir.weightSum_M_W.x <= 0.0f)
    {
        ReservoirClear(reservoir);
        return;
    }

    // Preserve represented average candidate weight when M is capped.
    const float clampedM =
        min(sourceM, safeMaxM);

    const float mScale =
        clampedM / sourceM;

    reservoir.weightSum_M_W.x *=
        mScale;

    reservoir.weightSum_M_W.y =
        clampedM;

    const float finalWeight =
        reservoir.weightSum_M_W.x /
        max(
            RT_RESTIR_MIN_TARGET,
            clampedM * target);

    if (!RtReservoirFiniteScalar(finalWeight) ||
        finalWeight <= 0.0f)
    {
        ReservoirClear(reservoir);
        return;
    }

    if (mathMode == RT_RESTIR_MATH_REFERENCE)
    {
        // Unclamped diagnostic mode. This still uses the R1 cosine-retargeted
        // receiver target and must not be described as an unbiased reference.
        reservoir.weightSum_M_W.z =
            finalWeight;
    }
    else
    {
        const float safeMaxWeight =
            max(1.0f, maxWeight);

        reservoir.weightSum_M_W.z =
            min(
                safeMaxWeight,
                finalWeight);
    }

    reservoir.flags |=
    RT_RESTIR_RESERVOIR_VALID;

    if (!ReservoirFinalizedValid(reservoir))
    {
        ReservoirClear(reservoir);
        return;
    }
}
#endif

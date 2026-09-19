#ifndef RT_RESERVOIR_HLSLI
#define RT_RESERVOIR_HLSLI

#include "Common.hlsli"

static const uint RT_RESTIR_RESERVOIR_VALID = 1u << 0;
static const uint RT_RESTIR_RESERVOIR_VISIBLE = 1u << 1;
static const uint RT_RESTIR_RESERVOIR_FALLBACK = 1u << 2;
static const uint RT_RESTIR_RESERVOIR_REPROJECTED = 1u << 3;
static const uint RT_RESTIR_RESERVOIR_SPATIAL = 1u << 4;
static const uint RT_RESTIR_RESERVOIR_M_CLAMPED = 1u << 5;
static const uint RT_RESTIR_RESERVOIR_W_CLAMPED = 1u << 6;
static const uint RT_RESTIR_RESERVOIR_TARGET_EXACT = 1u << 7;

static const uint RT_RESTIR_RESERVOIR_CLAMP_MASK =
    RT_RESTIR_RESERVOIR_M_CLAMPED |
    RT_RESTIR_RESERVOIR_W_CLAMPED;

static const uint RT_RESTIR_MATH_REFERENCE = 0u;
static const uint RT_RESTIR_MATH_ROBUST = 1u;

static const uint RT_RESTIR_M_MASK = 0x00000FFFu;
static const uint RT_RESTIR_AGE_MASK = 0x000FF000u;
static const uint RT_RESTIR_FLAGS_MASK = 0x0FF00000u;

static const uint RT_RESTIR_MAX_PACKED_M = 4095u;
static const uint RT_RESTIR_MAX_PACKED_AGE = 255u;

static const float RT_RESTIR_MIN_PDF = 1.0e-8f;
static const float RT_RESTIR_MIN_TARGET = 1.0e-8f;

struct RtRestirEnvReservoirPacked
{
    uint packedDirection;
    uint sampleIndex;

    float sourcePdf;
    float selectedTarget;

    float weightSum;
    float finalWeight;

    uint packedState;
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

float RtReservoirRand01(inout uint state)
{
    return float(RtReservoirLcg(state) & 0x00FFFFFFu) / 16777216.0f;
}

float2 RestirSignNotZero(float2 value)
{
    return float2(
        value.x >= 0.0f ? 1.0f : -1.0f,
        value.y >= 0.0f ? 1.0f : -1.0f);
}

uint PackRestirDirection(float3 direction)
{
    float3 n =
        SafeNormalize(direction);

    n /=
        max(
            abs(n.x) +
            abs(n.y) +
            abs(n.z),
            1.0e-8f);

    float2 oct = n.xy;

    if (n.z < 0.0f)
    {
        oct =
            (1.0f.xx - abs(oct.yx)) *
            RestirSignNotZero(oct);
    }

    const int2 encoded =
        int2(
            round(
                clamp(
                    oct,
                    -1.0f.xx,
                    1.0f.xx) *
                32767.0f));

    return
        (uint(encoded.x) & 0xFFFFu) |
        ((uint(encoded.y) & 0xFFFFu) << 16u);
}

float3 DecodeRestirDirection(uint packed)
{
    const int encodedX =
        (int) (packed << 16u) >> 16;

    const int encodedY =
        (int) packed >> 16;

    const float2 oct =
        float2(
            encodedX,
            encodedY) /
        32767.0f;

    float3 direction =
        float3(
            oct,
            1.0f -
            abs(oct.x) -
            abs(oct.y));

    if (direction.z < 0.0f)
    {
        direction.xy =
            (1.0f.xx -
                abs(direction.yx)) *
            RestirSignNotZero(
                direction.xy);
    }

    return SafeNormalize(direction);
}

uint ReservoirM(
    RtRestirEnvReservoirPacked reservoir)
{
    return
        reservoir.packedState &
        RT_RESTIR_M_MASK;
}

uint ReservoirAge(
    RtRestirEnvReservoirPacked reservoir)
{
    return
        (reservoir.packedState >> 12u) &
        0xFFu;
}

uint ReservoirFlags(
    RtRestirEnvReservoirPacked reservoir)
{
    return
        (reservoir.packedState >> 20u) &
        0xFFu;
}

void ReservoirSetState(
    inout RtRestirEnvReservoirPacked reservoir,
    uint M,
    uint age,
    uint flags)
{
    reservoir.packedState =
        min(M, RT_RESTIR_MAX_PACKED_M) |
        (min(age, RT_RESTIR_MAX_PACKED_AGE) << 12u) |
        ((flags & 0xFFu) << 20u);
}

void ReservoirAddRepresentedZeroWeightM(
    inout RtRestirEnvReservoirPacked reservoir,
    uint additionalM,
    uint representedAge,
    uint representedFlags,
    float maxM)
{
    if (additionalM == 0u)
        return;

    const uint oldM =
        ReservoirM(reservoir);

    const uint effectiveMaxM =
        min(
            uint(max(1.0f, maxM)),
            RT_RESTIR_MAX_PACKED_M);

    const uint proposedM =
        oldM + additionalM;

    const uint storedM =
        min(
            proposedM,
            effectiveMaxM);

    // Preserve the represented average weight when M is capped.
    if (storedM < proposedM &&
        reservoir.weightSum > 0.0f)
    {
        reservoir.weightSum *=
            float(storedM) /
            float(proposedM);
    }

    uint flags =
        ReservoirFlags(reservoir) |
        (representedFlags &
            RT_RESTIR_RESERVOIR_CLAMP_MASK);

    if (storedM < proposedM)
    {
        flags |=
            RT_RESTIR_RESERVOIR_M_CLAMPED;
    }

    ReservoirSetState(
        reservoir,
        storedM,
        max(
            ReservoirAge(reservoir),
            representedAge),
        flags);
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
bool ReservoirSampleValid(
    RtRestirEnvReservoirPacked reservoir)
{
    const uint flags =
        ReservoirFlags(reservoir);

    return
        (flags &
            RT_RESTIR_RESERVOIR_VALID) != 0u &&
        RtReservoirFiniteScalar(
            reservoir.sourcePdf) &&
        RtReservoirFiniteScalar(
            reservoir.selectedTarget) &&
        reservoir.sourcePdf >
            RT_RESTIR_MIN_PDF &&
        reservoir.selectedTarget >
            RT_RESTIR_MIN_TARGET;
}

bool ReservoirFinalizedValid(
    RtRestirEnvReservoirPacked reservoir)
{
    return
        ReservoirSampleValid(reservoir) &&
        RtReservoirFiniteScalar(
            reservoir.weightSum) &&
        RtReservoirFiniteScalar(
            reservoir.finalWeight) &&
        reservoir.weightSum > 0.0f &&
        reservoir.finalWeight > 0.0f &&
        ReservoirM(reservoir) > 0u;
}

void ReservoirClear(
    out RtRestirEnvReservoirPacked reservoir)
{
    reservoir.packedDirection = 0u;
    reservoir.sampleIndex = 0u;

    reservoir.sourcePdf = 0.0f;
    reservoir.selectedTarget = 0.0f;

    reservoir.weightSum = 0.0f;
    reservoir.finalWeight = 0.0f;

    reservoir.packedState = 0u;
    reservoir.surfaceId = 0xFFFFFFFFu;
}

float ReservoirTarget(RtRestirEnvReservoirPacked reservoir)
{
    return
        RtReservoirFiniteScalar(
            reservoir.selectedTarget)
        ? max(
            0.0f,
            reservoir.selectedTarget)
        : 0.0f;
}

float ReservoirSourcePdf(
    RtRestirEnvReservoirPacked reservoir)
{
    return
        RtReservoirFiniteScalar(
            reservoir.sourcePdf)
        ? max(
            0.0f,
            reservoir.sourcePdf)
        : 0.0f;
}

float ReservoirCandidateWeight(
    float target,
    float sourcePdf)
{
    if (!RtReservoirFiniteScalar(target) ||
        !RtReservoirFiniteScalar(sourcePdf) ||
        target <= RT_RESTIR_MIN_TARGET ||
        sourcePdf <= RT_RESTIR_MIN_PDF)
    {
        return 0.0f;
    }

    const float weight =
        target / sourcePdf;

    return
        RtReservoirFiniteScalar(weight) &&
        weight > 0.0f
        ? weight
        : 0.0f;
}

float ReservoirReuseWeight(
    RtRestirEnvReservoirPacked reservoir,
    float currentTarget,
    uint candidateM)
{
    if (!ReservoirFinalizedValid(reservoir) ||
        !RtReservoirFiniteScalar(currentTarget) ||
        currentTarget <= RT_RESTIR_MIN_TARGET ||
        candidateM == 0u)
    {
        return 0.0f;
    }

    const float weight =
        reservoir.finalWeight *
        currentTarget *
        float(candidateM);

    return
        RtReservoirFiniteScalar(weight) &&
        weight > 0.0f
        ? weight
        : 0.0f;
}

bool ReservoirRetarget(
    inout RtRestirEnvReservoirPacked reservoir,
    float receiverTarget,
    uint receiverSurfaceId)
{
    if (!ReservoirFinalizedValid(reservoir) ||
        !RtReservoirFiniteScalar(receiverTarget) ||
        receiverTarget <= RT_RESTIR_MIN_TARGET)
    {
        return false;
    }

    reservoir.selectedTarget =
        receiverTarget;

    reservoir.surfaceId =
        receiverSurfaceId;

    const uint flags =
        ReservoirFlags(reservoir) |
        RT_RESTIR_RESERVOIR_TARGET_EXACT;

    ReservoirSetState(
        reservoir,
        ReservoirM(reservoir),
        ReservoirAge(reservoir),
        flags);

    return true;
}

RtRestirEnvReservoirPacked MakeRestirCandidate(
    float3 wi,
    float sourcePdf,
    float target,
    uint sampleIndex,
    uint surfaceId)
{
    RtRestirEnvReservoirPacked candidate;
    ReservoirClear(candidate);

    if (any(isnan(wi)) ||
        any(isinf(wi)) ||
        dot(wi, wi) <= 1.0e-8f ||
        !RtReservoirFiniteScalar(sourcePdf) ||
        !RtReservoirFiniteScalar(target) ||
        sourcePdf <= RT_RESTIR_MIN_PDF ||
        target <= RT_RESTIR_MIN_TARGET)
    {
        return candidate;
    }

    candidate.packedDirection =
        PackRestirDirection(wi);

    candidate.sampleIndex =
        sampleIndex;

    candidate.sourcePdf =
        sourcePdf;

    candidate.selectedTarget =
        target;

    candidate.weightSum = 0.0f;
    candidate.finalWeight = 0.0f;

    candidate.surfaceId =
        surfaceId;

    ReservoirSetState(
        candidate,
        1u,
        0u,
        RT_RESTIR_RESERVOIR_VALID |
        RT_RESTIR_RESERVOIR_TARGET_EXACT);

    return candidate;
}

RtReservoirUpdateResult ReservoirUpdateWeightedTracked(
    inout RtRestirEnvReservoirPacked reservoir,
    RtRestirEnvReservoirPacked candidate,
    float candidateWeight,
    uint candidateM,
    inout uint rng)
{
    RtReservoirUpdateResult result;
    result.accepted = 0u;
    result.selected = 0u;

    if (!ReservoirSampleValid(candidate) ||
        !RtReservoirFiniteScalar(candidateWeight) ||
        candidateWeight <= 0.0f ||
        candidateM == 0u)
    {
        return result;
    }

    const float oldWeightSum =
        max(
            0.0f,
            reservoir.weightSum);

    const float proposedWeightSum =
        oldWeightSum +
        candidateWeight;

    const uint oldM =
        ReservoirM(reservoir);

    const uint proposedM =
        oldM + candidateM;

    if (!RtReservoirFiniteScalar(
            proposedWeightSum) ||
        proposedWeightSum <= 0.0f ||
        proposedM == 0u)
    {
        ReservoirClear(reservoir);
        return result;
    }

    const uint storedM =
        min(
            proposedM,
            RT_RESTIR_MAX_PACKED_M);

    const float storageScale =
        float(storedM) /
        float(proposedM);

    const float storedWeightSum =
        proposedWeightSum *
        storageScale;

    if (!RtReservoirFiniteScalar(
            storedWeightSum) ||
        storedWeightSum <= 0.0f)
    {
        ReservoirClear(reservoir);
        return result;
    }

    const float selectionProbability =
        saturate(
            candidateWeight /
            proposedWeightSum);
    
    const uint representedClampFlags =
        (ReservoirFlags(reservoir) |
         ReservoirFlags(candidate)) &
        RT_RESTIR_RESERVOIR_CLAMP_MASK;

    uint selectedAge =
        ReservoirAge(reservoir);

    uint selectedFlags =
        ReservoirFlags(reservoir);

    if (RtReservoirRand01(rng) <
        selectionProbability)
    {
        reservoir.packedDirection =
            candidate.packedDirection;

        reservoir.sampleIndex =
            candidate.sampleIndex;

        reservoir.sourcePdf =
            candidate.sourcePdf;

        reservoir.selectedTarget =
            candidate.selectedTarget;

        reservoir.surfaceId =
            candidate.surfaceId;

        selectedAge =
            ReservoirAge(candidate);

        selectedFlags =
            ReservoirFlags(candidate);

        result.selected = 1u;
    }
    selectedFlags |= representedClampFlags;

    if (storedM < proposedM)
    {
        selectedFlags |= RT_RESTIR_RESERVOIR_M_CLAMPED;
    }

    reservoir.weightSum =
        storedWeightSum;

    reservoir.finalWeight =
        0.0f;

    ReservoirSetState(
        reservoir,
        storedM,
        selectedAge,
        selectedFlags |
        RT_RESTIR_RESERVOIR_VALID);

    result.accepted = 1u;
    return result;
}

void ReservoirFinalize(
    inout RtRestirEnvReservoirPacked reservoir,
    float configuredMaxM,
    float configuredMaxWeight,
    uint mathMode)
{
    const float target =
        ReservoirTarget(reservoir);

    const uint sourceM =
        ReservoirM(reservoir);

    const uint effectiveMaxM =
        min(
            uint(max(
                1.0f,
                configuredMaxM)),
            RT_RESTIR_MAX_PACKED_M);

    if (!ReservoirSampleValid(reservoir) ||
        !RtReservoirFiniteScalar(
            reservoir.weightSum) ||
        reservoir.weightSum <= 0.0f ||
        target <= RT_RESTIR_MIN_TARGET ||
        sourceM == 0u)
    {
        ReservoirClear(reservoir);
        return;
    }

    const uint clampedM =
        min(
            sourceM,
            effectiveMaxM);

    uint flags =
        ReservoirFlags(reservoir);

    if (clampedM < sourceM)
    {
        const float mScale =
            float(clampedM) /
            float(sourceM);

        reservoir.weightSum *=
            mScale;

        flags |=
            RT_RESTIR_RESERVOIR_M_CLAMPED;
    }

    const float finalWeight =
        reservoir.weightSum /
        max(
            RT_RESTIR_MIN_TARGET,
            float(clampedM) *
            target);

    if (!RtReservoirFiniteScalar(finalWeight) ||
        finalWeight <= 0.0f)
    {
        ReservoirClear(reservoir);
        return;
    }

    if (mathMode ==
        RT_RESTIR_MATH_REFERENCE)
    {
        reservoir.finalWeight =
            finalWeight;
    }
    else
    {
        const float safeMaxWeight =
            max(
                1.0f,
                configuredMaxWeight);

        reservoir.finalWeight =
            min(
                finalWeight,
                safeMaxWeight);

        if (finalWeight >
            safeMaxWeight)
        {
            flags |=
                RT_RESTIR_RESERVOIR_W_CLAMPED;
        }
    }

    ReservoirSetState(
        reservoir,
        clampedM,
        ReservoirAge(reservoir),
        flags |
        RT_RESTIR_RESERVOIR_VALID);

    if (!ReservoirFinalizedValid(reservoir))
    {
        ReservoirClear(reservoir);
    }
}

#endif

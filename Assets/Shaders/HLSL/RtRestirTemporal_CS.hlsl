#include "Common.hlsli"
#include "RtReservoir.hlsli"
#include "RtRestirEnvironment.hlsli"
#include "RtRestirTarget.hlsli"
#include "RtRestirRejection.hlsli"
#include "RtPrimaryJitter.hlsli"

// RT DebugView ownership for this pass:
//   106 = previous-frame reservoir reuse accepted mask
//   107 = R: normalized M, G: normalized age, B: reuse confidence
//   118 = temporal rejection reasons

StructuredBuffer<RtRestirEnvReservoirPacked> g_CurrScratchReservoir : register(t0);
StructuredBuffer<RtRestirEnvReservoirPacked> g_PrevTemporalReservoir : register(t1);

Texture2D<float4> g_CurrNormal : register(t2);
Texture2D<float> g_CurrDepth : register(t3);
Texture2D<float2> g_CurrPrevUV : register(t4);
Texture2D<float> g_CurrViewZ : register(t5);
Texture2D<uint> g_CurrSurfaceId : register(t6);

Texture2D<float4> g_PrevNormal : register(t7);
Texture2D<float> g_PrevDepth : register(t8);
Texture2D<float> g_PrevViewZ : register(t9);
Texture2D<uint> g_PrevSurfaceId : register(t10);

Texture2D<float4> g_CurrDiffuseAlbedo : register(t11);
Texture2D<float4> g_CurrSpecularF0 : register(t12);
Texture2D<float4> g_RestirEnvironmentRadiance : register(t13);
Texture2D<float4> g_CurrRestirReceiver : register(t14);

RWStructuredBuffer<RtRestirEnvReservoirPacked> g_OutTemporalReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);
RWTexture2D<float> g_OutTemporalConfidence : register(u2);
RWTexture2D<uint> g_OutRejectionReason : register(u3);

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
    
    // Receiver reconstruction and exact-target evaluation.
    row_major float4x4 InverseViewProjection;

    float DeltaRoughnessCutoff;
    uint PrimarySampleIndex;
    uint PrimaryResetId;
    uint PrimaryJitterEnabled;

    uint HasEnvironmentRadiance;
    float LightingIntensity;
    float LightingRotationRadians;
    uint EnvironmentFaceSize;
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

RtRestirReceiver LoadTemporalReceiver(
    uint2 pixel)
{
    RtRestirReceiver receiver;

    const float4 receiverNormalRoughness =
        g_CurrRestirReceiver[pixel];

    const float4 diffuseAlbedo =
        g_CurrDiffuseAlbedo[pixel];

    const float4 specularF0 =
        g_CurrSpecularF0[pixel];

    receiver.normal =
        SafeNormalize(
            receiverNormalRoughness.xyz *
            2.0f -
            1.0f);

    const float2 primaryJitter =
        PrimaryJitterEnabled != 0u
        ? RtPrimaryPixelJitter(
            pixel,
            PrimarySampleIndex,
            PrimaryResetId)
        : 0.0f.xx;

    receiver.viewDirection =
        ReconstructRestirPrimaryViewDirection(
            pixel,
            InvResolution,
            primaryJitter,
            InverseViewProjection);

    receiver.diffuseAlbedo =
        max(
            diffuseAlbedo.rgb,
            0.0f.xxx);

    receiver.specularF0 =
        max(
            specularF0.rgb,
            0.0f.xxx);

    receiver.materialRoughness =
        saturate(
            receiverNormalRoughness.a);

    receiver.shadingRoughness =
        PbrShadingRoughnessFromMaterial(
            receiver.materialRoughness);
    
    receiver.surfaceId =
        g_CurrSurfaceId[pixel];

    receiver.valid =
        receiver.surfaceId !=
            SURFACE_ID_INVALID &&
        specularF0.a > 0.0f &&
        RestirFinite3(receiver.normal) &&
        RestirFinite3(receiver.viewDirection) &&
        RestirFinite3(receiver.diffuseAlbedo) &&
        RestirFinite3(receiver.specularF0)
        ? 1u
        : 0u;

    receiver.specularEligible =
        RestirSpecularEligibleFromAov(
            receiver.materialRoughness,
            DeltaRoughnessCutoff)
        ? 1u
        : 0u;

    return receiver;
}

bool CurrentGuideValid(
    uint2 pixel,
    out uint rejectionReason)
{
    rejectionReason = 0u;

    const uint surfaceId =
        g_CurrSurfaceId[pixel];

    const float depth =
        g_CurrDepth[pixel];

    const float viewZ =
        g_CurrViewZ[pixel];

    if (!SurfaceIdValid(surfaceId))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_SURFACE_ID;
    }

    if (!RtReservoirFiniteScalar(depth) ||
        depth >= 0.9999f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_DEPTH;
    }

    if (!DistanceValid(viewZ))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_VIEWZ;
    }

    return rejectionReason == 0u;
}

bool EvaluateTemporalGuideWeight(
    uint2 currPixel,
    uint2 prevPixel,
    out float reuseWeight,
    out uint rejectionReason)
{
    rejectionReason = 0u;
    reuseWeight = 0.0f;

    if (SurfaceIdHistoryValid == 0u)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_INVALID_HISTORY |
            RT_RESTIR_REJECT_SURFACE_ID;
    }

    if (ViewZHistoryValid == 0u)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_INVALID_HISTORY |
            RT_RESTIR_REJECT_VIEWZ;
    }

    const uint currId = g_CurrSurfaceId[currPixel];
    const uint prevId = g_PrevSurfaceId[prevPixel];

    if (!SurfaceIdValid(currId) ||
        !SurfaceIdValid(prevId) ||
        currId != prevId)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_SURFACE_ID;
    }

    const float currDepth = g_CurrDepth[currPixel];
    const float prevDepth = g_PrevDepth[prevPixel];

    if (!RtReservoirFiniteScalar(currDepth) ||
        !RtReservoirFiniteScalar(prevDepth) ||
        currDepth >= 0.9999f ||
        prevDepth >= 0.9999f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_DEPTH;
    }

    const float currViewZ = g_CurrViewZ[currPixel];
    const float prevViewZ = g_PrevViewZ[prevPixel];

    if (!DistanceValid(currViewZ) || !DistanceValid(prevViewZ))
    {
        rejectionReason |= RT_RESTIR_REJECT_VIEWZ;
    }

    const float4 currNR = g_CurrNormal[currPixel];
    const float4 prevNR = g_PrevNormal[prevPixel];
    const float3 currNormal = UnpackNormal(currNR);
    const float3 prevNormal = UnpackNormal(prevNR);
    const float normalDot = saturate(dot(currNormal, prevNormal));

    if (!RtReservoirFiniteScalar(normalDot) ||
            normalDot < 0.95f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_NORMAL;
    }

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
    
    if (!RtReservoirFiniteScalar(normalWeight))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_NORMAL;
    }

    if (!RtReservoirFiniteScalar(depthWeight))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_DEPTH;
    }

    if (!RtReservoirFiniteScalar(roughnessWeight))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_ROUGHNESS;
    }

    if (!RtReservoirFiniteScalar(viewZWeight))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_VIEWZ;
    }

    if (rejectionReason != 0u)
    {
        return false;
    }

    reuseWeight =
        normalWeight *
        depthWeight *
        roughnessWeight *
        viewZWeight;

    if (!RtReservoirFiniteScalar(reuseWeight) ||
            reuseWeight <= 0.0f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_CONFIDENCE;

        reuseWeight = 0.0f;
        return false;
    }

    return true;
}

bool EvaluatePreviousAtCurrentReceiver(
    inout RtRestirEnvReservoirPacked previous,
    uint2 currentPixel,
    out RtRestirTargetEvaluation evaluation,
    out float sourceToReceiverRatio)
{
    evaluation =
        RestirZeroTarget();

    sourceToReceiverRatio =
        0.0f;

    if (!ReservoirFinalizedValid(previous))
        return false;

    const RtRestirReceiver receiver =
        LoadTemporalReceiver(
            currentPixel);

    if (receiver.valid == 0u)
        return false;

    const float3 wi =
        DecodeRestirDirection(
            previous.packedDirection);

    const float3 Li =
        SampleRestirEnvironmentRadiance(
            g_RestirEnvironmentRadiance,
            wi,
            HasEnvironmentRadiance,
            EnvironmentFaceSize,
            LightingIntensity,
            LightingRotationRadians);

    evaluation =
        EvaluateRestirEnvironmentTarget(
            receiver,
            wi,
            Li);

    if (evaluation.combinedTarget <=
        RT_RESTIR_MIN_TARGET)
    {
        return false;
    }

    sourceToReceiverRatio =
        previous.selectedTarget /
        max(
            RT_RESTIR_MIN_TARGET,
            evaluation.combinedTarget);

    if (!ReservoirRetarget(
            previous,
            evaluation.combinedTarget,
            receiver.surfaceId))
    {
        return false;
    }

    return true;
}

float RestirTargetRatioConfidence(
    float targetRatio)
{
    return exp(
        -abs(
            log2(
                max(
                    targetRatio,
                    1.0e-4f))));
}

float ComputeTemporalRestirConfidence(
    RtRestirEnvReservoirPacked reservoir,
    float guideWeight,
    float targetRatio)
{
    if (!ReservoirFinalizedValid(reservoir))
        return 0.0f;

    const float mConfidence =
        saturate(
            float(ReservoirM(reservoir)) /
            max(1.0f, MaxM));

    const float ageConfidence =
        MaxAge > 0.0f
        ? 1.0f -
            saturate(
                float(ReservoirAge(reservoir)) /
                MaxAge)
        : 1.0f;

    const float ratioConfidence =
        RestirTargetRatioConfidence(
            targetRatio);

    return saturate(
        guideWeight *
        lerp(0.35f, 1.0f, mConfidence) *
        lerp(0.50f, 1.0f, ageConfidence) *
        ratioConfidence);
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
    const RtRestirReceiver currentReceiver =
        LoadTemporalReceiver(pixel);

    const RtRestirEnvReservoirPacked current =
        g_CurrScratchReservoir[pixelIndex];

    RtRestirEnvReservoirPacked outputReservoir;
    ReservoirClear(outputReservoir);
    
    // An empty current reservoir can still represent candidate attempts with
    // zero total weight. Seed output M with those attempts so that, if temporal
    // history supplies a valid selected sample, final W is normalized by both
    // the current zero-weight candidates and the reused history.
    const uint currentRepresentedM =
        ReservoirM(current);

    if (!ReservoirFinalizedValid(current) &&
        !ReservoirSampleValid(current) &&
        currentReceiver.valid != 0u &&
        currentRepresentedM > 0u)
    {
        ReservoirAddRepresentedZeroWeightM(
            outputReservoir,
            currentRepresentedM,
            ReservoirAge(current),
            ReservoirFlags(current),
            MaxM);

        outputReservoir.surfaceId = currentReceiver.surfaceId;
    }

    
    uint rejectionReason = 0u;

    if (currentReceiver.valid == 0u)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_RECEIVER;
    }

    uint rng =
        HashUintRtRestir(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0xB5297A4Du);

    bool usedPrevious = false;

    // Defaults describe a valid current-frame-only reservoir:
    // no guide disagreement and no target-ratio disagreement.
    float confidenceGuideWeight = 1.0f;
    float confidenceTargetRatio = 1.0f;

    if (ReservoirFinalizedValid(current))
    {
        const uint currentM =
            max(
                1u,
                min(
                    ReservoirM(current),
                    uint(MaxM)));

        const float currentWeight =
            ReservoirReuseWeight(
                current,
                ReservoirTarget(current),
                currentM);

        ReservoirUpdateWeightedTracked(
            outputReservoir,
            current,
            currentWeight,
            currentM,
            rng);
    }
            
    // Evaluate all applicable temporal-reuse conditions so debug 118 can
    // contain multiple simultaneous rejection causes.
    const bool historyAvailable =
        TemporalEnabled != 0u &&
        HistoryValid != 0u;

    if (!historyAvailable)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_INVALID_HISTORY;
    }

    uint currentGuideReason = 0u;

    const bool currentGuideValid =
        CurrentGuideValid(
            pixel,
            currentGuideReason);

    rejectionReason |=
        currentGuideReason;

    if (historyAvailable)
    {
        const float2 prevUV =
            g_CurrPrevUV[pixel];

        const bool uvValid =
            PrevUVValid(prevUV);

        if (!uvValid)
        {
            rejectionReason |=
                RT_RESTIR_REJECT_INVALID_UV;
        }

        if (uvValid)
        {
            const uint2 prevPixel =
                min(
                    uint2(
                        prevUV *
                        float2(width, height)),
                    uint2(
                        width - 1u,
                        height - 1u));

            const uint prevIndex =
                prevPixel.y * width +
                prevPixel.x;

            RtRestirEnvReservoirPacked previous =
                g_PrevTemporalReservoir[prevIndex];

            const bool previousValid =
                ReservoirFinalizedValid(previous);

            const uint previousRepresentedM = ReservoirM(previous);

            const bool previousMOnly =
                !previousValid &&
                !ReservoirSampleValid(previous) &&
                previousRepresentedM > 0u;

            if (!previousValid && !previousMOnly)
            {
                rejectionReason |= RT_RESTIR_REJECT_INVALID_HISTORY;
            }
            
            const bool previousAgeValid =
                (previousValid || previousMOnly) &&
                float(ReservoirAge(previous)) <
                    MaxAge;

            if ((previousValid || previousMOnly) && !previousAgeValid)
            {
                rejectionReason |= RT_RESTIR_REJECT_AGE;
            }

            float guideWeight = 0.0f;
            uint guideReason = 0u;

            const bool guideValid =
                EvaluateTemporalGuideWeight(
                    pixel,
                    prevPixel,
                    guideWeight,
                    guideReason);

            rejectionReason |=
                guideReason;

            const bool confidenceValid =
                guideValid &&
                RtReservoirFiniteScalar(guideWeight) &&
                guideWeight >= ReprojectMinWeight;          

            const bool previousMOnlyHistory =
                previousMOnly &&
                previousAgeValid;

            if (previousMOnlyHistory &&
                currentReceiver.valid != 0u &&
                currentGuideValid &&
                guideValid &&
                confidenceValid)
            {
                const uint carriedAge =
                    min(
                        ReservoirAge(previous) + 1u,
                        RT_RESTIR_MAX_PACKED_AGE);

                ReservoirAddRepresentedZeroWeightM(
                    outputReservoir,
                    previousRepresentedM,
                    carriedAge,
                    ReservoirFlags(previous),
                    MaxM);
            }

            if (!confidenceValid)
            {
                rejectionReason |=
                    RT_RESTIR_REJECT_CONFIDENCE;
            }

            RtRestirTargetEvaluation evaluation =
                RestirZeroTarget();

            float sourceToReceiverRatio = 0.0f;

            bool targetValid = false;

            if (previousValid &&
                currentReceiver.valid != 0u)
            {
                targetValid =
                    EvaluatePreviousAtCurrentReceiver(
                        previous,
                        pixel,
                        evaluation,
                        sourceToReceiverRatio);

                if (!targetValid)
                {
                    rejectionReason |=
                        RT_RESTIR_REJECT_TARGET;
                }
            }

            const bool reuseAccepted =
                currentGuideValid &&
                previousValid &&
                previousAgeValid &&
                guideValid &&
                confidenceValid &&
                targetValid;

            if (reuseAccepted)
            {
                const uint previousAge =
                    min(
                        ReservoirAge(previous) + 1u,
                        RT_RESTIR_MAX_PACKED_AGE);

                const uint previousFlags =
                    ReservoirFlags(previous) |
                    RT_RESTIR_RESERVOIR_REPROJECTED;

                ReservoirSetState(
                    previous,
                    ReservoirM(previous),
                    previousAge,
                    previousFlags);

                const uint previousM =
                    max(
                        1u,
                        min(
                            ReservoirM(previous),
                            uint(MaxM)));

                const float previousWeight =
                    ReservoirReuseWeight(
                        previous,
                        evaluation.combinedTarget,
                        previousM);

                if (!RtReservoirFiniteScalar(
                    previousWeight) || previousWeight <= 0.0f)
                {
                    rejectionReason |= RT_RESTIR_REJECT_TARGET;
                }
                else
                {
                    // Guide confidence controls reuse eligibility and diagnostics;
                    // it does not scale represented energy.
                    const RtReservoirUpdateResult updateResult =
                        ReservoirUpdateWeightedTracked(
                            outputReservoir,
                            previous,
                            previousWeight,
                            previousM,
                            rng);

                    if (updateResult.accepted != 0u)
                    {
                        // Debug 106 indicates that temporal history participated.
                        usedPrevious = true;

                        // Age advances whenever temporal history participates,
                        // regardless of which sample RIS ultimately selects.
                        const uint temporalFlags =
                            ReservoirFlags(outputReservoir) |
                            RT_RESTIR_RESERVOIR_REPROJECTED;

                        ReservoirSetState(
                            outputReservoir,
                            ReservoirM(outputReservoir),
                            previousAge,
                            temporalFlags);
                    }

                    if (updateResult.selected != 0u)
                    {
                        // Confidence describes the sample selected by RIS.
                        confidenceGuideWeight =
                            saturate(guideWeight);

                        confidenceTargetRatio =
                            sourceToReceiverRatio;
                    }
                }
            }
        }
    }
    
    // Preserve M-only state across frames. A reservoir with represented M but
    // no selected sample is intentionally not finalized-valid, but its zero-weight
    // candidate attempts still belong in future temporal normalization.
    const bool hadSampleBeforeFinalize = ReservoirSampleValid(outputReservoir);

    const uint representedMBeforeFinalize = ReservoirM(outputReservoir);

    const uint representedAgeBeforeFinalize = ReservoirAge(outputReservoir);

    const uint representedFlagsBeforeFinalize = ReservoirFlags(outputReservoir);

    ReservoirFinalize(
        outputReservoir,
        MaxM,
        MaxWeight,
        MathMode);

    if (!ReservoirFinalizedValid(outputReservoir))
    {
        ReservoirClear(outputReservoir);

        if (!hadSampleBeforeFinalize &&
            currentReceiver.valid != 0u &&
            representedMBeforeFinalize > 0u)
        {
            ReservoirSetState(
                outputReservoir,
                representedMBeforeFinalize,
                representedAgeBeforeFinalize,
                representedFlagsBeforeFinalize &
                    RT_RESTIR_RESERVOIR_CLAMP_MASK);

            outputReservoir.surfaceId = currentReceiver.surfaceId;
        }
    }

    if (ReservoirFinalizedValid(outputReservoir))
    {
        outputReservoir.surfaceId =
            currentReceiver.surfaceId;
    }

    const float temporalConfidence =
        ComputeTemporalRestirConfidence(
            outputReservoir,
            confidenceGuideWeight,
            confidenceTargetRatio);

    g_OutTemporalReservoir[pixelIndex] =
        outputReservoir;

    g_OutTemporalConfidence[pixel] =
        temporalConfidence;

    g_OutRejectionReason[pixel] =
        rejectionReason;

    if (DebugView == 106u)
    {
        g_Output[pixel] =
            float4((usedPrevious ? 1.0f : 0.0f).xxx, 1.0f);
    }
    else if (DebugView == 107u)
    {
        const bool valid =
            ReservoirFinalizedValid(outputReservoir);

        const float normalizedM =
            valid
                ? saturate(
                    float(ReservoirM(outputReservoir)) /
                    max(1.0f, MaxM))
                : 0.0f;

        const float normalizedAge =
            valid && MaxAge > 0.0f
                ? saturate(
                    float(ReservoirAge(outputReservoir)) /
                    MaxAge)
                : 0.0f;

        const float confidence =
            valid
                ? temporalConfidence
                : 0.0f;

        g_Output[pixel] = float4(normalizedM, normalizedAge, confidence, 1.0f);
    }
    else if (DebugView == 118u)
    {
        g_Output[pixel] =
            float4(
                RestirRejectionDebugColor(
                    rejectionReason),
                1.0f);
    }
}

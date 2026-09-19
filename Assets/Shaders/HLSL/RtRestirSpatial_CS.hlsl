#include "Common.hlsli"
#include "RtReservoir.hlsli"
#include "RtRestirEnvironment.hlsli"
#include "RtRestirTarget.hlsli"
#include "RtRestirRejection.hlsli"
#include "RtPrimaryJitter.hlsli"

// RT DebugView ownership for this pass:
//   108 = accepted spatial reuse count
//   109 = selected neighbour distance
//   115 = exact receiver combined target
//   116 = source / receiver target ratio
//   117 = receiver reuse candidate weight
//   118 = combined temporal/spatial rejection reason

StructuredBuffer<RtRestirEnvReservoirPacked> g_TemporalReservoir : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<uint> g_CurrSurfaceId : register(t3);
Texture2D<float> g_CurrViewZ : register(t4);
Texture2D<float4> g_CurrDiffuseAlbedo : register(t5);
Texture2D<float4> g_CurrSpecularF0 : register(t6);
Texture2D<float4> g_RestirEnvironmentRadiance : register(t7);
Texture2D<float> g_TemporalConfidence : register(t8);
Texture2D<uint> g_TemporalRejectionReason : register(t9);
Texture2D<float4> g_CurrRestirReceiver : register(t10);

RWStructuredBuffer<RtRestirEnvReservoirPacked> g_OutSpatialReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);
RWTexture2D<float> g_OutRestirConfidence : register(u2);
RWTexture2D<uint> g_OutRejectionReason : register(u3);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;
static const float RT_RESTIR_SPATIAL_PI = 3.14159265358979323846f;

static const float2 kRestirNeighbourDisk[32] =
{
    float2(0.1768f, 0.1768f),
    float2(-0.2270f, 0.2917f),
    float2(0.0348f, -0.3960f),
    float2(0.3907f, 0.0915f),
    float2(-0.4295f, -0.1373f),
    float2(0.2518f, -0.4234f),
    float2(-0.1062f, 0.5091f),
    float2(0.5315f, -0.1915f),
    float2(-0.5528f, 0.2122f),
    float2(0.1185f, 0.6126f),
    float2(-0.3252f, -0.5637f),
    float2(0.6640f, 0.0207f),
    float2(-0.6062f, -0.3260f),
    float2(0.4449f, 0.5471f),
    float2(-0.0802f, -0.7238f),
    float2(0.6913f, -0.2812f),
    float2(-0.7258f, 0.2771f),
    float2(0.2585f, 0.7459f),
    float2(-0.4827f, -0.6357f),
    float2(0.8074f, 0.1020f),
    float2(-0.7474f, -0.3785f),
    float2(0.5269f, -0.6718f),
    float2(-0.1748f, 0.8480f),
    float2(0.8663f, -0.1740f),
    float2(-0.8625f, 0.2365f),
    float2(0.3922f, 0.8130f),
    float2(-0.6041f, -0.6925f),
    float2(0.9152f, 0.0414f),
    float2(-0.8280f, -0.4238f),
    float2(0.6418f, -0.6782f),
    float2(-0.2354f, 0.9179f),
    float2(0.9466f, -0.2168f)
};

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
    out float viewZ,
    out uint rejectionReason)
{
    rejectionReason = 0u;
    
    const float4 packedNormalRoughness = g_CurrNormal[pixel];

    normal = UnpackNormal(packedNormalRoughness);
    roughness = saturate(packedNormalRoughness.a);
    depth = g_CurrDepth[pixel];
    surfaceId = g_CurrSurfaceId[pixel];
    viewZ = g_CurrViewZ[pixel];

    if (!SurfaceIdValid(surfaceId))
    {
        rejectionReason |=
            RT_RESTIR_REJECT_SURFACE_ID;
    }

    if (!RtReservoirFiniteScalar(depth) || depth >= 0.9999f)
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

RtRestirReceiver LoadSpatialReceiver(
    uint2 pixel)
{
    RtRestirReceiver receiver;

    const float4 normalRoughness =
        g_CurrRestirReceiver[pixel];

    const float4 diffuseAlbedo =
        g_CurrDiffuseAlbedo[pixel];

    const float4 specularF0 =
        g_CurrSpecularF0[pixel];

    receiver.normal =
        SafeNormalize(
            normalRoughness.xyz *
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
            normalRoughness.a);

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

bool EvaluateSpatialGuideWeight(
    uint2 centerPixel,
    uint2 neighbourPixel,
    out float guideWeight,
    out uint rejectionReason)
{
    guideWeight = 0.0f;
    rejectionReason = 0u;

    float3 centerNormal;
    float centerRoughness;
    float centerDepth;
    uint centerSurfaceId;
    float centerViewZ;
    uint centerReason = 0u;

    const bool centerValid =
        LoadSpatialGuide(
            centerPixel,
            centerNormal,
            centerRoughness,
            centerDepth,
            centerSurfaceId,
            centerViewZ,
            centerReason);

    rejectionReason |= centerReason;

    float3 neighbourNormal;
    float neighbourRoughness;
    float neighbourDepth;
    uint neighbourSurfaceId;
    float neighbourViewZ;
    uint neighbourReason = 0u;

    const bool neighbourValid =
        LoadSpatialGuide(
            neighbourPixel,
            neighbourNormal,
            neighbourRoughness,
            neighbourDepth,
            neighbourSurfaceId,
            neighbourViewZ,
            neighbourReason);

    rejectionReason |= neighbourReason;

    // SurfaceId is intentionally a hard gate. It prevents reuse across object
    // and material boundaries even when all continuous guides look similar.
    if (centerSurfaceId != neighbourSurfaceId)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_SURFACE_ID;
    }

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

    if (!RtReservoirFiniteScalar(normalWeight) ||
        normalWeight < 0.05f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_NORMAL;
    }

    if (!RtReservoirFiniteScalar(depthWeight) ||
        depthWeight < 0.05f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_DEPTH;
    }

    if (!RtReservoirFiniteScalar(roughnessWeight) ||
        roughnessWeight < 0.05f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_ROUGHNESS;
    }

    if (!RtReservoirFiniteScalar(viewZWeight) ||
        viewZWeight < 0.05f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_VIEWZ;
    }
    
    if (!centerValid ||
        !neighbourValid ||
        rejectionReason != 0u)
    {
        return false;
    }

    guideWeight =
        normalWeight *
        depthWeight *
        roughnessWeight *
        viewZWeight;

    if (!RtReservoirFiniteScalar(guideWeight) || guideWeight <= 0.0f)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_CONFIDENCE;

        guideWeight = 0.0f;
        return false;
    }

    return true;
}

bool EvaluateNeighbourAtCenterReceiver(
    inout RtRestirEnvReservoirPacked neighbour,
    RtRestirReceiver centerReceiver,
    out RtRestirTargetEvaluation evaluation,
    out float sourceToReceiverRatio)
{
    evaluation =
        RestirZeroTarget();

    sourceToReceiverRatio =
        0.0f;

    if (!ReservoirFinalizedValid(neighbour) ||
        centerReceiver.valid == 0u)
    {
        return false;
    }

    const float3 wi =
        DecodeRestirDirection(
            neighbour.packedDirection);

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
            centerReceiver,
            wi,
            Li);

    if (evaluation.combinedTarget <=
        RT_RESTIR_MIN_TARGET)
    {
        return false;
    }

    sourceToReceiverRatio =
        neighbour.selectedTarget /
        max(
            RT_RESTIR_MIN_TARGET,
            evaluation.combinedTarget);

    return ReservoirRetarget(
        neighbour,
        evaluation.combinedTarget,
        centerReceiver.surfaceId);
}

float RestirSpatialTargetRatioConfidence(
    float targetRatio)
{
    return exp(
        -abs(
            log2(
                max(
                    targetRatio,
                    1.0e-4f))));
}

RtReservoirUpdateResult UpdateSpatialReservoir(
    inout RtRestirEnvReservoirPacked destination,
    RtRestirEnvReservoirPacked candidate,
    float candidateWeight,
    uint candidateM,
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

    RtRestirEnvReservoirPacked outputReservoir;
    ReservoirClear(outputReservoir);

    RtRestirEnvReservoirPacked center =
        g_TemporalReservoir[pixelIndex];

    const RtRestirReceiver centerReceiver =
        LoadSpatialReceiver(pixel);
    
    // The temporal center can intentionally contain no selected sample while
    // still representing zero-weight candidate attempts. Spatial reuse must
    // preserve that represented M; otherwise pixels filled by neighbours are
    // conditioned on the center having produced a successful sample.
    const uint centerRepresentedM =
        min(
            ReservoirM(center),
            uint(MaxM));
    
    const bool centerValid = ReservoirFinalizedValid(center);

    const bool centerMOnly =
        !centerValid &&
        !ReservoirSampleValid(center) &&
        centerRepresentedM > 0u;

    if (centerMOnly && centerReceiver.valid != 0u)
    {
        ReservoirSetState(
            outputReservoir,
            centerRepresentedM,
            ReservoirAge(center),
            ReservoirFlags(center) &
                RT_RESTIR_RESERVOIR_CLAMP_MASK);

        outputReservoir.surfaceId = centerReceiver.surfaceId;
    }

    uint rng =
        HashUintRtSpatial(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0x8DA6B343u);

    uint acceptedCount = 0u;
    float selectedDistance = 0.0f;

    float selectedReceiverTarget = 0.0f;
    float selectedTargetRatio = 1.0f;
    float selectedReuseWeight = 0.0f;
    
    // Spatial confidence starts from the center pixel's temporal confidence.
    const float centerTemporalConfidence =
        saturate(
            g_TemporalConfidence[pixel]);

    float outputConfidence = 0.0f;

    g_OutRestirConfidence[pixel] = 0.0f;
    uint rejectionReason =
        g_TemporalRejectionReason[pixel];
    
    if (centerReceiver.valid == 0u)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_RECEIVER;
    }

    if (!centerValid && !centerMOnly)
    {
        rejectionReason |=
            RT_RESTIR_REJECT_INVALID_HISTORY;
    }

    if (centerReceiver.valid != 0u &&
        centerValid)
    {
        const uint centerM =
            max(
                1u,
                min(
                    ReservoirM(center),
                    uint(MaxM)));

        const float centerWeight =
            ReservoirReuseWeight(
                center,
                ReservoirTarget(center),
                centerM);

        const RtReservoirUpdateResult centerUpdateResult =
        UpdateSpatialReservoir(
            outputReservoir,
            center,
            centerWeight,
            centerM,
            0.0f,
            selectedDistance,
            rng);

        if (centerUpdateResult.selected != 0u)
        {
            outputConfidence =
                centerTemporalConfidence;
            
            selectedReceiverTarget =
                ReservoirTarget(center);

            // The center reservoir is already expressed at this receiver.
            selectedTargetRatio = 1.0f;

            selectedReuseWeight =
                centerWeight;
        }
    }

    const uint sampleCount = max(1u, SampleCount);
    const uint radius = max(1u, Radius);
    
    // Per-pixel/per-frame permutation prevents every pixel from walking the
    // low-discrepancy sequence from the same entry.
    const uint sequenceOffset =
    HashUintRtSpatial(
        pixel.x ^
        pixel.y * 4099u ^
        FrameIndex * 131071u) &
    31u;

    // Rotate the complete disk sequence once per pixel/frame.
    const float sequenceAngle =
        RtReservoirRand01(rng) *
        2.0f *
        RT_RESTIR_SPATIAL_PI;

    const float sequenceSin =
        sin(sequenceAngle);

    const float sequenceCos =
        cos(sequenceAngle);

    [loop]
    for (uint sampleIndex = 0u;
        sampleIndex < sampleCount;
        ++sampleIndex)
    {
        const float2 baseOffset =
            kRestirNeighbourDisk[
                (sampleIndex +
                 sequenceOffset) &
                31u];

        const float2 rotatedOffset =
            float2(
                sequenceCos * baseOffset.x -
                sequenceSin * baseOffset.y,
                sequenceSin * baseOffset.x +
                sequenceCos * baseOffset.y);

        const int2 offset =
            int2(
                round(
                    rotatedOffset *
                    float(radius)));

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

        RtRestirEnvReservoirPacked neighbour =
            g_TemporalReservoir[neighbourIndex];

        float guideWeight = 0.0f;

        RtRestirTargetEvaluation evaluation =
            RestirZeroTarget();

        float sourceToReceiverRatio = 0.0f;       

        uint candidateReason = 0u;

        const bool neighbourValid = ReservoirFinalizedValid(neighbour);

        const uint neighbourRepresentedM =
            min(
                ReservoirM(neighbour),
                uint(MaxM));

        const bool neighbourMOnly =
            !neighbourValid &&
            !ReservoirSampleValid(neighbour) &&
            neighbourRepresentedM > 0u;

        // An M-only neighbour is not a usable lighting sample, but it is still a
        // represented set of zero-weight proposals. Do not reject its M merely
        // because it has no selected sample.
        if (!neighbourValid && !neighbourMOnly)
        {
            candidateReason |= RT_RESTIR_REJECT_INVALID_HISTORY;
        }

        uint guideReason = 0u;

        const bool guideValid =
            EvaluateSpatialGuideWeight(
                pixel,
                neighbourPixel,
                guideWeight,
                guideReason);

        candidateReason |= guideReason;

        if (centerReceiver.valid == 0u)
        {
            candidateReason |= RT_RESTIR_REJECT_RECEIVER;
        }

        if (!guideValid || guideWeight < SpatialMinReuseWeight)
        {
            candidateReason |= RT_RESTIR_REJECT_CONFIDENCE;
        }

        // If this neighbour contains represented M but no selected sample, it
        // contributes zero weight but still participates in M whenever the same
        // spatial compatibility gates would have admitted the neighbour.
        if (neighbourMOnly)
        {
            if (candidateReason == 0u)
            {
                ReservoirAddRepresentedZeroWeightM(
                    outputReservoir,
                    neighbourRepresentedM,
                    ReservoirAge(neighbour),
                    ReservoirFlags(neighbour),
                    MaxM);
            }
            else
            {
                rejectionReason |=
                    candidateReason;
            }

            continue;
        }

        bool targetValid = false;

        if (neighbourValid && centerReceiver.valid != 0u)
        {
            targetValid =
                EvaluateNeighbourAtCenterReceiver(
                    neighbour,
                    centerReceiver,
                    evaluation,
                    sourceToReceiverRatio);

            if (!targetValid)
            {
                candidateReason |= RT_RESTIR_REJECT_TARGET;
            }
        }

        if (candidateReason != 0u)
        {
            rejectionReason |=
                candidateReason;
            continue;
        }

        ReservoirSetState(
            neighbour,
            ReservoirM(neighbour),
            ReservoirAge(neighbour),
            ReservoirFlags(neighbour) |
                RT_RESTIR_RESERVOIR_SPATIAL);

        const uint neighbourM =
            max(
                1u,
                min(
                    ReservoirM(neighbour),
                    uint(MaxM)));

        const float neighbourWeight =
            ReservoirReuseWeight(
                neighbour,
                evaluation.combinedTarget,
                neighbourM);

        if (!RtReservoirFiniteScalar(neighbourWeight) ||
            neighbourWeight <= 0.0f)
        {
            rejectionReason |=
                RT_RESTIR_REJECT_TARGET;

            continue;
        }
        
        // Confidence measures guide and retargeting trust.
        // It does not alter neighbourWeight.
        const float ratioConfidence =
            RestirSpatialTargetRatioConfidence(
                sourceToReceiverRatio);
        
        const float neighbourTemporalConfidence =
            saturate(
                g_TemporalConfidence[
                    neighbourPixel]);

        const float candidateConfidence =
            saturate(
                neighbourTemporalConfidence *
                guideWeight *
                ratioConfidence);

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
        
        if (updateResult.selected != 0u)
        {
            outputConfidence =
                candidateConfidence;
            
            selectedReceiverTarget =
                evaluation.combinedTarget;

            selectedTargetRatio =
                sourceToReceiverRatio;

            selectedReuseWeight =
                neighbourWeight;
        }
    }

    ReservoirFinalize(outputReservoir, MaxM, MaxWeight, MathMode);

    if (!ReservoirFinalizedValid(outputReservoir))
    {
        ReservoirClear(outputReservoir);
    }
    else
    {
        outputReservoir.surfaceId =
            centerReceiver.surfaceId;
    }

    g_OutSpatialReservoir[pixelIndex] =
        outputReservoir;

    g_OutRestirConfidence[pixel] =
        ReservoirFinalizedValid(outputReservoir)
        ? saturate(outputConfidence)
        : 0.0f;

    g_OutRejectionReason[pixel] =
        rejectionReason;

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
    else if (DebugView == 115u)
    {
        // Exact combined target evaluated at the current receiver.
        const float value =
            selectedReceiverTarget /
            (1.0f + selectedReceiverTarget);

        g_Output[pixel] =
            float4(
                Heat(value),
                1.0f);
    }
    else if (DebugView == 116u)
    {
        // Stored source target / current receiver target.
        //
        // ratio < 1 : source underestimated receiver -> blue
        // ratio = 1 : exact agreement               -> green
        // ratio > 1 : source overestimated receiver -> red
        //
        // Four log2 stops reaches full divergence colour:
        // 1 / 16 -> blue
        // 1      -> green
        // 16     -> red
        const float safeRatio =
            max(
                selectedTargetRatio,
                1.0e-4f);

        const float signedLogRatio =
            log2(safeRatio);

        const float magnitude =
            saturate(
                abs(signedLogRatio) /
                4.0f);

        const float3 divergenceColor =
            signedLogRatio < 0.0f
            ? float3(0.0f, 0.0f, 1.0f)
            : float3(1.0f, 0.0f, 0.0f);

        const float3 color =
            lerp(
                float3(0.0f, 1.0f, 0.0f),
                divergenceColor,
                magnitude);

        g_Output[pixel] =
            float4(
                color,
                1.0f);
    }
    else if (DebugView == 117u)
    {
        // Actual receiver reuse candidate weight presented to RIS.
        const float value =
            selectedReuseWeight /
            (1.0f + selectedReuseWeight);

        g_Output[pixel] =
            float4(
                Heat(value),
                1.0f);
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

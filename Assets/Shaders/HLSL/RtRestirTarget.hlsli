#ifndef RT_RESTIR_TARGET_HLSLI
#define RT_RESTIR_TARGET_HLSLI

#include "Common.hlsli"
#include "PBR.hlsli"
#include "RtRestirReceiver.hlsli"

static const float kPi = 3.14159265f;

struct RtRestirReceiver
{
    float3 normal;
    float3 viewDirection;

    // This is baseColor * (1 - metallic), not raw base color.
    float3 diffuseAlbedo;

    // Fully evaluated current textured F0:
    // lerp(0.04, baseColor, metallic).
    float3 specularF0;

    float materialRoughness;
    float shadingRoughness;
    uint surfaceId;
    uint valid;
    uint specularEligible;
};

struct RtRestirTargetEvaluation
{
    float3 diffuse;
    float3 specular;

    float diffuseTarget;
    float specularTarget;
    float combinedTarget;

    float diffuseShare;
    float specularShare;
};

bool RestirFiniteScalar(float value)
{
    return !isnan(value) && !isinf(value);
}

bool RestirFinite3(float3 value)
{
    return
        !any(isnan(value)) &&
        !any(isinf(value));
}

float RestirSafeLuminance(float3 value)
{
    if (!RestirFinite3(value))
        return 0.0f;

    return max(
        0.0f,
        dot(
            max(value, 0.0f.xxx),
            float3(
                0.2126f,
                0.7152f,
                0.0722f)));
}

RtRestirTargetEvaluation RestirZeroTarget()
{
    RtRestirTargetEvaluation result;

    result.diffuse = 0.0f.xxx;
    result.specular = 0.0f.xxx;

    result.diffuseTarget = 0.0f;
    result.specularTarget = 0.0f;
    result.combinedTarget = 0.0f;

    result.diffuseShare = 0.0f;
    result.specularShare = 0.0f;

    return result;
}

RtRestirTargetEvaluation EvaluateRestirEnvironmentTarget(
    RtRestirReceiver receiver,
    float3 wi,
    float3 Li)
{
    RtRestirTargetEvaluation result =
        RestirZeroTarget();

    if (receiver.valid == 0u ||
        !RestirFinite3(receiver.normal) ||
        !RestirFinite3(receiver.viewDirection) ||
        !RestirFinite3(receiver.diffuseAlbedo) ||
        !RestirFinite3(receiver.specularF0) ||
        !RestirFinite3(wi) ||
        !RestirFinite3(Li))
    {
        return result;
    }

    const float3 N =
        SafeNormalize(receiver.normal);

    const float3 V =
        SafeNormalize(receiver.viewDirection);

    const float3 L =
        SafeNormalize(wi);

    const float NoL =
        saturate(dot(N, L));
    
    if (NoL <= 1.0e-4f)
    {
        return result;
    }
    
    const PbrEnvironmentBrdfSplit brdf =
        EvaluateEnvironmentBrdfSplit(
            receiver.diffuseAlbedo,
            receiver.specularF0,
            receiver.shadingRoughness,
            N,
            V,
            L);

    const float3 safeLi =
        max(Li, 0.0f.xxx);

    result.diffuse =
        safeLi *
        brdf.diffuse *
        NoL;

    if (receiver.specularEligible != 0u)
    {
        result.specular =
            safeLi *
            brdf.specular *
            NoL;
    }

    if (!RestirFinite3(result.diffuse))
        result.diffuse = 0.0f.xxx;

    if (!RestirFinite3(result.specular))
        result.specular = 0.0f.xxx;

    result.diffuse =
        max(result.diffuse, 0.0f.xxx);

    result.specular =
        max(result.specular, 0.0f.xxx);

    result.diffuseTarget =
        RestirSafeLuminance(
            result.diffuse);

    result.specularTarget =
        RestirSafeLuminance(
            result.specular);

    result.combinedTarget =
        RestirSafeLuminance(
            result.diffuse +
            result.specular);

    if (result.combinedTarget > 1.0e-8f)
    {
        result.diffuseShare =
            saturate(
                result.diffuseTarget /
                result.combinedTarget);

        result.specularShare =
            saturate(
                result.specularTarget /
                result.combinedTarget);
    }

    return result;
}

float3 ReconstructRestirPrimaryViewDirection(
    uint2 pixel,
    float2 invResolution,
    float2 jitter,
    row_major float4x4 inverseViewProjection)
{
    const float2 uv =
        (float2(pixel) +
         0.5f.xx +
         jitter) *
        invResolution;

    const float2 ndc =
        float2(
            uv.x * 2.0f - 1.0f,
            1.0f - uv.y * 2.0f);

    float4 nearP =
        mul(
            float4(
                ndc,
                0.0f,
                1.0f),
            inverseViewProjection);

    float4 farP =
        mul(
            float4(
                ndc,
                1.0f,
                1.0f),
            inverseViewProjection);

    const float nearW =
        abs(nearP.w) > 1.0e-6f
        ? nearP.w
        : (nearP.w >= 0.0f
            ? 1.0e-6f
            : -1.0e-6f);

    const float farW =
        abs(farP.w) > 1.0e-6f
        ? farP.w
        : (farP.w >= 0.0f
            ? 1.0e-6f
            : -1.0e-6f);

    nearP.xyz /= nearW;
    farP.xyz /= farW;

    return
        SafeNormalize(
            nearP.xyz -
            farP.xyz);
}

#endif

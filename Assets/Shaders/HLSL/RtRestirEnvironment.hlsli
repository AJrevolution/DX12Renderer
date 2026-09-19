#ifndef RT_RESTIR_ENVIRONMENT_HLSLI
#define RT_RESTIR_ENVIRONMENT_HLSLI

#include "Common.hlsli"
#include "RtSampling.hlsli"

float3 RestirRotateDirectionY(
    float3 direction,
    float radians)
{
    const float s = sin(radians);
    const float c = cos(radians);

    return float3(
        c * direction.x + s * direction.z,
        direction.y,
        -s * direction.x + c * direction.z);
}

float3 RestirWorldToEnvironmentDirection(
    float3 worldDirection,
    float lightingRotationRadians)
{
    return RestirRotateDirectionY(
        SafeNormalize(worldDirection),
        lightingRotationRadians);
}

float3 RestirEnvironmentToWorldDirection(
    float3 environmentDirection,
    float lightingRotationRadians)
{
    // Inverse of RestirWorldToEnvironmentDirection().
    return RestirRotateDirectionY(
        SafeNormalize(environmentDirection),
        -lightingRotationRadians);
}

uint2 RestirEnvironmentAtlasTexel(
    float3 environmentDirection,
    uint faceSize)
{
    float2 faceUv =
        0.0f.xx;

    const uint face =
        DirectionToCubeFaceUV(
            SafeNormalize(
                environmentDirection),
            faceUv);

    const uint safeFaceSize =
        max(
            1u,
            faceSize);

    const uint2 faceTexel =
        min(
            uint2(
                faceUv *
                float(safeFaceSize)),
            uint2(
                safeFaceSize - 1u,
                safeFaceSize - 1u));

    return
        uint2(
            faceTexel.x,
            face *
                safeFaceSize +
                faceTexel.y);
}

float3 SampleRestirEnvironmentRadiance(
    Texture2D<float4> radianceTexture,
    float3 worldDirection,
    uint hasRadianceTexture,
    uint faceSize,
    float lightingIntensity,
    float lightingRotationRadians)
{
    if (hasRadianceTexture == 0u ||
        faceSize == 0u)
    {
        return 0.0f.xxx;
    }

    uint textureWidth;
    uint textureHeight;

    radianceTexture.GetDimensions(
        textureWidth,
        textureHeight);

    if (textureWidth < faceSize ||
        textureHeight < faceSize * 6u)
    {
        return 0.0f.xxx;
    }

    const float3 environmentDirection =
        RestirWorldToEnvironmentDirection(
            worldDirection,
            lightingRotationRadians);

    const uint2 atlasTexel =
        RestirEnvironmentAtlasTexel(
            environmentDirection,
            faceSize);

    const float3 radiance =
        radianceTexture.Load(
            int3(
                atlasTexel,
                0)).rgb;

    if (any(isnan(radiance)) ||
        any(isinf(radiance)))
    {
        return 0.0f.xxx;
    }

    return
        max(radiance, 0.0f.xxx) *
        max(lightingIntensity, 0.0f);
}
#endif

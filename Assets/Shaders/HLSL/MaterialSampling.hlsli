#ifndef ASSETS_SHADERS_HLSL_MATERIAL_SAMPLING_HLSLI
#define ASSETS_SHADERS_HLSL_MATERIAL_SAMPLING_HLSLI

#include "Common.hlsli"
#include "MaterialConstants.hlsli"

float2 SelectUv(uint texCoord, float2 uv0, float2 uv1)
{
    return texCoord == 1u ? uv1 : uv0;
}

void ApplyAlphaMask(uint alphaMode, float alpha, float alphaCutoff)
{
    if (alphaMode == ALPHA_MODE_MASK && alpha < alphaCutoff)
    {
        discard;
    }
}

float3 DecodeNormalScaled(float3 texel, float normalScale)
{
    float3 n = texel * 2.0f - 1.0f;
    n.xy *= normalScale;
    return SafeNormalize(n);
}

float ComputeMaterialOcclusion(float occlusionTexel, float occlusionStrength)
{
    return saturate(
        1.0f +
        saturate(occlusionStrength) *
        (occlusionTexel - 1.0f));
}

float3 ComputeEmissive(
    float3 emissiveFactor,
    uint hasEmissiveTexture,
    float3 emissiveTexel)
{
    return hasEmissiveTexture != 0u
        ? emissiveFactor * emissiveTexel
        : emissiveFactor;
}

#endif
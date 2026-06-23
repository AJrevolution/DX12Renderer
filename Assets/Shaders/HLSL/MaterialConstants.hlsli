#ifndef ASSETS_SHADERS_HLSL_MATERIAL_CONSTANTS_HLSLI
#define ASSETS_SHADERS_HLSL_MATERIAL_CONSTANTS_HLSLI

#define ALPHA_MODE_OPAQUE 0u
#define ALPHA_MODE_MASK   1u
#define ALPHA_MODE_BLEND  2u

cbuffer PerDrawConstants : register(b1)
{
    row_major float4x4 World;

    float4 BaseColorFactor;
    float4 EmissiveFactorAndAlphaCutoff;

    float MetallicFactor;
    float RoughnessFactor;
    float OcclusionStrength;
    float NormalScale;

    uint AlphaMode;
    uint DoubleSided;
    uint HasOcclusionTexture;
    uint HasEmissiveTexture;

    uint BaseColorTexCoord;
    uint NormalTexCoord;
    uint MetalRoughTexCoord;
    uint OcclusionTexCoord;

    uint EmissiveTexCoord;
    uint HasNormalTexture;
    uint MaterialIndex;
    uint PerDrawPad0;
};

#endif
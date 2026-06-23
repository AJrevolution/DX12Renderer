#pragma once
#include <DirectXMath.h>
#include <cstdint>
using namespace DirectX;

struct PerDrawConstants
{
    DirectX::XMFLOAT4X4 world;

    DirectX::XMFLOAT4 baseColorFactor;
    DirectX::XMFLOAT4 emissiveFactorAndAlphaCutoff; // rgb = emissiveFactor, a = alphaCutoff

    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float normalScale;

    uint32_t alphaMode;
    uint32_t doubleSided;
    uint32_t hasOcclusionTexture;
    uint32_t hasEmissiveTexture;

    uint32_t baseColorTexCoord;
    uint32_t normalTexCoord;
    uint32_t metalRoughTexCoord;
    uint32_t occlusionTexCoord;

    uint32_t emissiveTexCoord;
    uint32_t hasNormalTexture;
    uint32_t materialIndex;
    uint32_t perDrawPad0;
};
static_assert((sizeof(PerDrawConstants) % 16) == 0);

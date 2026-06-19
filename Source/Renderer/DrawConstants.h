#pragma once
#include <DirectXMath.h>
#include <cstdint>
using namespace DirectX;

struct PerDrawConstants
{
    XMFLOAT4X4 world;
    uint32_t materialIndex;
    uint32_t pad[3];

    XMFLOAT4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    uint32_t hasOcclusionTexture;
};
static_assert((sizeof(PerDrawConstants) % 16) == 0);

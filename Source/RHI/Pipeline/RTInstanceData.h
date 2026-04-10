#pragma once

#include <cstdint>
#include <DirectXMath.h>

struct RTInstanceData
{
    DirectX::XMFLOAT4 baseColorFactor; // rgb used, a reserved
    float metallic;
    float roughness;
    float pad0[2];

    uint32_t meshType;    // 0 = floor, 1 = quad
    uint32_t materialId;  // reserved for later
    uint32_t pad1[2];
};

static_assert((sizeof(RTInstanceData) % 16) == 0, "RTInstanceData must be 16-byte aligned");
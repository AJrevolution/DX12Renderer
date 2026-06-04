#pragma once

#include <cstdint>
#include <DirectXMath.h>

struct RTInstanceData
{
    DirectX::XMFLOAT4 baseColorFactor; // rgb used, a reserved
    float metallic;
    float roughness;
    float pad0[2];

    uint32_t meshType;          // 0 = floor, 1 = quad
    uint32_t materialId = 0;    // material table index
    uint32_t objectId = 0;      // stable per-draw/object id for SurfaceId
    uint32_t pad1 = 0;
    
    // Previous object-to-world transform for RT-only motion/prevUV AOV generation.
    // Stored as a full 4x4 for clarity and to match HLSL row_major float4x4 use.
    DirectX::XMFLOAT4X4 prevObjectToWorld{};
};

static_assert((sizeof(RTInstanceData) % 16) == 0, "RTInstanceData must be 16-byte aligned");
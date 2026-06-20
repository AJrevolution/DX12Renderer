#pragma once

#include <cstdint>
#include <DirectXMath.h>

struct RTInstanceData
{
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };

    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusionStrength = 1.0f;
    uint32_t hasOcclusionTexture = 0;

    // 0 = procedural floor
    // 1 = procedural quad
    // 2 = imported combined glTF mesh
    uint32_t meshType = 0;

    // Index into the DXR material texture table.
    uint32_t materialId = 0;

    // Stable object id for SurfaceId generation.
    uint32_t objectId = 0;

    // First index for this instance's submesh inside the selected index buffer.
    // Procedural floor/quad use 0. Imported glTF submeshes use Mesh::Submesh::indexStart.
    uint32_t indexStart = 0;

    DirectX::XMFLOAT4X4 prevObjectToWorld{};
};


static_assert((sizeof(RTInstanceData) % 16) == 0, "RTInstanceData must be 16-byte aligned");
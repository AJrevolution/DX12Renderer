#pragma once

#include <cstdint>
#include <DirectXMath.h>

struct RTInstanceData
{
    DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 emissiveFactorAndAlphaCutoff = { 0.0f, 0.0f, 0.0f, 0.5f };

    float metallic = 0.0f;
    float roughness = 0.5f;
    float occlusionStrength = 1.0f;
    float normalScale = 1.0f;

    uint32_t alphaMode = 0;
    uint32_t doubleSided = 0;
    uint32_t hasOcclusionTexture = 0;
    uint32_t hasEmissiveTexture = 0;

    uint32_t baseColorTexCoord = 0;
    uint32_t normalTexCoord = 0;
    uint32_t metalRoughTexCoord = 0;
    uint32_t occlusionTexCoord = 0;

    uint32_t emissiveTexCoord = 0;
    uint32_t hasNormalTexture = 0;

    // 0 = procedural floor
    // 1 = procedural quad
    // 2 = imported combined glTF mesh
    uint32_t meshType = 0;

    // Index into the DXR material texture table.
    uint32_t materialId = 0;

    // Index into the imported DXR mesh-buffer arrays.
    // Procedural floor/quad use 0.
    uint32_t meshBufferId = 0;

    // Stable object id for SurfaceId generation.
    uint32_t objectId = 0;

    // First index for this instance's submesh inside the selected index buffer.
    // Procedural floor/quad use 0. Imported glTF submeshes use Mesh::Submesh::indexStart.
    uint32_t indexStart = 0;

    uint32_t _pad0 = 0;

    DirectX::XMFLOAT4X4 prevObjectToWorld{};
};
static_assert((sizeof(RTInstanceData) % 16) == 0);

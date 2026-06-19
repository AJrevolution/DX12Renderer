#pragma once

#include "Common.h"
#include "Source/Scene/Mesh.h"
#include "Source/Scene/Material.h"
#include "Source/RHI/Resources/Texture.h"
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/RHI/CommandList/CommandList.h"

#include <DirectXMath.h>
#include <filesystem>
#include <string>
#include <vector>

class LoadedModel
{
public:
    struct Draw
    {
        DirectX::XMFLOAT4X4 world{};
        uint32_t submeshIndex = 0;
        uint32_t materialIndex = 0;
    };

    bool LoadGltf(
        ID3D12Device* device,
        CommandList& cl,
        UploadArena& upload,
        DescriptorAllocator& srvHeap,
        uint32_t frameIndex,
        const std::filesystem::path& path,
        const DirectX::XMMATRIX& importTransform = DirectX::XMMatrixIdentity());

    bool IsLoaded() const { return m_loaded; }

    Mesh& GetMesh() { return m_mesh; }
    const Mesh& GetMesh() const { return m_mesh; }

    Material* GetMaterial(uint32_t index);
    const Material* GetMaterial(uint32_t index) const;

    const std::vector<Draw>& Draws() const { return m_draws; }

    const std::wstring& LastError() const { return m_lastError; }

    void Clear();

private:
    Mesh m_mesh;
    std::vector<Material> m_materials;
    std::vector<Texture> m_textures;
    std::vector<Draw> m_draws;

    std::wstring m_lastError;
    bool m_loaded = false;
};
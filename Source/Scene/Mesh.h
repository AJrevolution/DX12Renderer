#pragma once
#include "Common.h"
#include "Source/RHI/Resources/GpuBuffer.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/RHI/CommandList/CommandList.h"

#include <cstdint>
#include <vector>

class Mesh
{
public:
    struct Vertex
    {
        float px, py, pz;    // Position
        float nx, ny, nz;    // Normal
        float tx, ty, tz, tw;    // Tangent.xyz + handedness.w
        float r, g, b, a;    // Color
        float u, v;          // UV
    };

    struct Submesh
    {
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        int32_t vertexBase = 0;
        uint32_t materialIndex = 0;
    };

    void CreateTexturedQuad(
        ID3D12Device* device,
        CommandList& cl,
        UploadArena& upload,
        uint32_t frameIndex);

    void CreateFloorPlane(
        ID3D12Device* device,
        CommandList& cl,
        UploadArena& upload,
        uint32_t frameIndex);

    // Generic imported-mesh path. Uses 32-bit indices by design because real
    // model assets can exceed 65k unique vertices once OBJ/glTF vertex tuples
    // are expanded.
    void CreateFromData(
        ID3D12Device* device,
        CommandList& cl,
        UploadArena& upload,
        uint32_t frameIndex,
        const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        const std::vector<Submesh>& submeshes,
        const wchar_t* debugName,
        bool generateTangents);


    const D3D12_VERTEX_BUFFER_VIEW& VBV() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& IBV() const { return m_ibv; }
    uint32_t IndexCount() const { return m_indexCount; }
    uint32_t VertexCount() const { return m_vertexCount; }

    ID3D12Resource* VertexBufferResource() const { return m_vb.Get(); }
    ID3D12Resource* IndexBufferResource() const { return m_ib.Get(); }

    uint32_t VertexStride() const { return sizeof(Vertex); }    
    DXGI_FORMAT IndexFormat() const { return m_ibv.Format; }
    uint32_t IndexBufferByteSize() const { return m_ibv.SizeInBytes; }

    uint32_t SubmeshCount() const
    {
        return static_cast<uint32_t>(m_submeshes.size());
    }

    const Submesh& GetSubmesh(uint32_t index) const
    {
        return index < m_submeshes.size()
            ? m_submeshes[index]
            : m_wholeMeshSubmesh;
    }

    const Submesh& WholeMeshSubmesh() const
    {
        return m_wholeMeshSubmesh;
    }

private:
    static void GenerateTangents(
        std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices);

    void SetSingleWholeMeshSubmesh();

private:
    GPUBuffer m_vb;
    GPUBuffer m_ib;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;

    Submesh m_wholeMeshSubmesh{};
    std::vector<Submesh> m_submeshes;

};

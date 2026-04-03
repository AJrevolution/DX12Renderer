#pragma once
#include "Common.h"
#include "Source/RHI/Resources/GpuBuffer.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/RHI/CommandList/CommandList.h"

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

    const D3D12_VERTEX_BUFFER_VIEW& VBV() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& IBV() const { return m_ibv; }
    uint32_t IndexCount() const { return m_indexCount; }

    ID3D12Resource* VertexBufferResource() const { return m_vb.Get(); }
    ID3D12Resource* IndexBufferResource() const { return m_ib.Get(); }

    uint32_t VertexStride() const { return sizeof(Vertex); }
    uint32_t VertexCount() const { return m_vbv.StrideInBytes ? (m_vbv.SizeInBytes / m_vbv.StrideInBytes) : 0; }
    DXGI_FORMAT IndexFormat() const { return m_ibv.Format; }
private:
    GPUBuffer m_vb;
    GPUBuffer m_ib;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};
    uint32_t m_indexCount = 0;

};

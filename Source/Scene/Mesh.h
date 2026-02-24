#pragma once
#include "Common.h"
#include "Source/RHI/Resources/GpuBuffer.h"
#include "Source/RHI/Memory/UploadArena.h"

class Mesh
{
public:
    struct Vertex
    {
        float px, py, pz;
        float r, g, b, a;
        float u, v;
    };

    void CreateTexturedQuad(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmd,
        UploadArena& upload,
        uint32_t frameIndex);

    const D3D12_VERTEX_BUFFER_VIEW& VBV() const { return m_vbv; }
    const D3D12_INDEX_BUFFER_VIEW& IBV() const { return m_ibv; }
    uint32_t IndexCount() const { return m_indexCount; }

private:
    GPUBuffer m_vb;
    GPUBuffer m_ib;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW  m_ibv{};
    uint32_t m_indexCount = 0;
};

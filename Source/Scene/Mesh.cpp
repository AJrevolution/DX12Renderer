#include "Mesh.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void Mesh::CreateTexturedQuad(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmd,
    UploadArena& upload,
    uint32_t frameIndex)
{
    const Vertex verts[] =
    {
        //  pos                 color            uv
        { -0.5f,  0.5f, 0.0f,   1,1,1,1,        0,0 }, // TL
        {  0.5f,  0.5f, 0.0f,   1,1,1,1,        1,0 }, // TR
        {  0.5f, -0.5f, 0.0f,   1,1,1,1,        1,1 }, // BR
        { -0.5f, -0.5f, 0.0f,   1,1,1,1,        0,1 }, // BL
    };

    const uint16_t indices[] =
    {
        0, 1, 2,
        0, 2, 3
    };

    const uint32_t vbSize = sizeof(verts);
    const uint32_t ibSize = sizeof(indices);
    m_indexCount = 6;

    // Create DEFAULT VB/IB in COPY_DEST
    m_vb.CreateDefaultBuffer(device, vbSize, D3D12_RESOURCE_STATE_COPY_DEST, L"VB: Quad (DEFAULT)");
    m_ib.CreateDefaultBuffer(device, ibSize, D3D12_RESOURCE_STATE_COPY_DEST, L"IB: Quad (DEFAULT)");

    // Stage vertex data
    auto vbAlloc = upload.Allocate(frameIndex, vbSize, 16);
    memcpy(vbAlloc.cpu, verts, vbSize);

    cmd->CopyBufferRegion(m_vb.Get(), 0, upload.GetBuffer(frameIndex), vbAlloc.offset, vbSize);

    // VB barrier
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_vb.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
        );
        cmd->ResourceBarrier(1, &b);
    }

    // Stage index data
    auto ibAlloc = upload.Allocate(frameIndex, ibSize, 16);
    memcpy(ibAlloc.cpu, indices, ibSize);

    cmd->CopyBufferRegion(m_ib.Get(), 0, upload.GetBuffer(frameIndex), ibAlloc.offset, ibSize);

    // IB barrier
    {
        D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            m_ib.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_INDEX_BUFFER
        );
        cmd->ResourceBarrier(1, &b);
    }

    // Views
    m_vbv.BufferLocation = m_vb.GPUAddress();
    m_vbv.SizeInBytes = vbSize;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib.GPUAddress();
    m_ibv.SizeInBytes = ibSize;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
}

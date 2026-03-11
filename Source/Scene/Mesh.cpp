#include "Mesh.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void Mesh::CreateTexturedQuad(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex)
{
    const Vertex verts[] =
    {
        // Position            Normal         Tangent(xyz,w)      Color         UV
        { -0.5f,  0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      0,0 }, // 0: Top-Left
        {  0.5f,  0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      1,0 }, // 1: Top-Right
        {  0.5f, -0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      1,1 }, // 2: Bottom-Right
        { -0.5f, -0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      0,1 }, // 3: Bottom-Left
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

    CommandList::SetGlobalState(m_vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    CommandList::SetGlobalState(m_ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

    // Stage vertex data
    auto vbAlloc = upload.Allocate(frameIndex, vbSize, 16);
    memcpy(vbAlloc.cpu, verts, vbSize);

    cl.CopyBuffer(m_vb.Get(), 0, upload.GetBuffer(frameIndex), vbAlloc.offset, vbSize);

    cl.Transition(m_vb.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Stage index data
    auto ibAlloc = upload.Allocate(frameIndex, ibSize, 16);
    memcpy(ibAlloc.cpu, indices, ibSize);

    cl.CopyBuffer(m_ib.Get(), 0, upload.GetBuffer(frameIndex), ibAlloc.offset, ibSize);

    cl.Transition(m_ib.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Views
    m_vbv.BufferLocation = m_vb.GPUAddress();
    m_vbv.SizeInBytes = vbSize;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib.GPUAddress();
    m_ibv.SizeInBytes = ibSize;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
}

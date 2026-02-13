#include "Source\Renderer\Passes\TrianglePass.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "Source\RHI\CommandList\CommandList.h"

void TrianglePass::Initialize(
    ID3D12Device* device, 
    DXGI_FORMAT rtvFormat, 
    const fs::path& shaderDir,
    ID3D12GraphicsCommandList* cmd,
    UploadArena& upload,
    uint32_t frameIndex)
{
    if (m_initialized)
        return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"Triangle_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"Triangle_PS.cso");

    m_rootSig.InitializeEmpty(device);
    m_pso.InitialiseTriangle(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat);

    //temp
    const Vertex verts[] =
    {
        {  0.0f,  0.5f, 0.0f, 1,0,0,1 },
        {  0.5f, -0.5f, 0.0f, 0,1,0,1 },
        { -0.5f, -0.5f, 0.0f, 0,0,1,1 },
    };
    const UINT vbSize = sizeof(verts);
    
    //Default heap
    m_vbDefault.CreateDefaultBuffer(device, vbSize, D3D12_RESOURCE_STATE_COPY_DEST, L"VB: Triangle (DEFAULT)");

    //Staging memory from UploadArena
    auto alloc = upload.Allocate(frameIndex, vbSize, 16);
    memcpy(alloc.cpu, verts, vbSize);
    
    CommandList cl(cmd);
    ////Copy from UploadArena into DEFAULT VB
    //cmd->CopyBufferRegion(
    //    m_vbDefault.Get(), 0,
    //    upload.GetBuffer(frameIndex), alloc.offset,
    //    vbSize
    //);

    cl.CopyBuffer(
        m_vbDefault.Get(), 0,
        upload.GetBuffer(frameIndex), alloc.offset,
        vbSize
    );
   
    //Barrier COPY_DEST -> VERTEX_BUFFER
    //{
    //    D3D12_RESOURCE_BARRIER b{};
    //    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    //    b.Transition.pResource = m_vbDefault.Get();
    //    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    //    b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    //    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    //    cmd->ResourceBarrier(1, &b);
    //}

    cl.Transition(
        m_vbDefault.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
    );

    //Build VBV
    m_vbView.BufferLocation = m_vbDefault.GPUAddress();
    m_vbView.SizeInBytes = vbSize;
    m_vbView.StrideInBytes = sizeof(Vertex);

    m_initialized = true;
}

void TrianglePass::Render(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height)
{
    const D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);

    cmd->DrawInstanced(3, 1, 0, 0);
}

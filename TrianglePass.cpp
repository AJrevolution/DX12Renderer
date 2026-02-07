#include "Source\Renderer\Passes\TrianglePass.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void TrianglePass::Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, const fs::path& shaderDir)
{
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
    
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_vb)),
        "CreateCommittedResource(VB)"
    );

    SetD3D12ObjectName(m_vb.Get(), L"VB: Triangle");

    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, 0 };
    ThrowIfFailed(m_vb->Map(0, &readRange, &mapped), "VB Map");
    memcpy(mapped, verts, vbSize);
    m_vb->Unmap(0, nullptr);

    m_vbView.BufferLocation = m_vb->GetGPUVirtualAddress();
    m_vbView.SizeInBytes = vbSize;
    m_vbView.StrideInBytes = sizeof(Vertex);
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

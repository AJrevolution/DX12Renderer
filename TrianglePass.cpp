#include "Source\Renderer\Passes\TrianglePass.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "Source\RHI\CommandList\CommandList.h"

void TrianglePass::Initialize(
    ID3D12Device* device, 
    DXGI_FORMAT rtvFormat, 
    const fs::path& shaderDir)
{
    if (m_initialized)
        return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"Triangle_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"Triangle_PS.cso");

    m_rootSig.InitializeMain (device);
    m_pso.InitialiseTriangle(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat);

    m_initialized = true;
}

void TrianglePass::Render(ID3D12GraphicsCommandList* cmd, 
    uint32_t width,
    uint32_t height, 
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
    D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,
    const Material& material,
    const Mesh& mesh)
{
    const D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    // Root params:
    // 0: b0 per-frame
    // 1: b1 per-draw
    // 2: descriptor table (space1) for material SRVs
    cmd->SetGraphicsRootConstantBufferView(0, perFrameCb);
    cmd->SetGraphicsRootConstantBufferView(1, perDrawCb);
    cmd->SetGraphicsRootDescriptorTable(2, material.baseColorSrv.gpu);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &mesh.VBV());
    cmd->IASetIndexBuffer(&mesh.IBV());

    cmd->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}

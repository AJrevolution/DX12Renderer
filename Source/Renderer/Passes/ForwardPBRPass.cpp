#include "ForwardPBRPass.h"

void ForwardPBRPass::Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const fs::path& shaderDir)
{
    if (m_initialized) return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"ForwardPBR_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"ForwardPBR_FS.cso");

    m_rootSig.InitializeForwardPBRV2(device);

    m_pso.InitialiseForwardPBR(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat, dsvFormat);

    m_initialized = true;
}

void ForwardPBRPass::Render(
    CommandList& cl,
    uint32_t width,
    uint32_t height,
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
    D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneTable,
    const Material& material,
    const Mesh& mesh)
{
    auto* cmd = cl.Get();

    const D3D12_VIEWPORT vp{ 0,0,(float)width,(float)height,0,1 };
    const D3D12_RECT sc{ 0,0,(LONG)width,(LONG)height };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    cmd->SetGraphicsRootConstantBufferView(0, perFrameCb);
    cmd->SetGraphicsRootConstantBufferView(1, perDrawCb);

    cmd->SetGraphicsRootDescriptorTable(2, sceneTable);
    cmd->SetGraphicsRootDescriptorTable(3, material.table.gpu);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &mesh.VBV());
    cmd->IASetIndexBuffer(&mesh.IBV());

    cmd->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}


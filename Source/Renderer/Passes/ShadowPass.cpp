#include "ShadowPass.h"

void ShadowPass::Initialize(ID3D12Device* device, DXGI_FORMAT dsvFormat, const fs::path& shaderDir)
{
    if (m_initialized)
        return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"Shadow_VS.cso");

    m_rootSig.InitializeForwardPBRV2(device);
    m_pso.InitialiseShadow(device, m_rootSig.Get(), vs.GetBytecode(), dsvFormat);

    m_initialized = true;
}

void ShadowPass::Render(
    CommandList& cl,
    uint32_t shadowSize,
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
    D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,
    const Mesh& mesh)
{
    auto* cmd = cl.Get();

    const D3D12_VIEWPORT vp{ 0, 0, (float)shadowSize, (float)shadowSize, 0.0f, 1.0f };
    const D3D12_RECT sc{ 0, 0, (LONG)shadowSize, (LONG)shadowSize };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    cmd->SetGraphicsRootConstantBufferView(0, perFrameCb);
    cmd->SetGraphicsRootConstantBufferView(1, perDrawCb);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &mesh.VBV());
    cmd->IASetIndexBuffer(&mesh.IBV());

    cmd->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}
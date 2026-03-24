#include "Source/Renderer/Passes/DeferredLightingPass.h"

void DeferredLightingPass::Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, const fs::path& shaderDir)
{
    if (m_initialized) return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"DeferredLight_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"DeferredLight_PS.cso");

    m_rootSig.InitializeForwardPBRV2(device);
    m_pso.InitialiseDeferredLight(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat);

    m_initialized = true;
}

void DeferredLightingPass::Render(
    CommandList& cl,
    uint32_t width,
    uint32_t height,
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneTable,
    D3D12_GPU_DESCRIPTOR_HANDLE deferredInputTable)
{
    auto* cmd = cl.Get();

    const D3D12_VIEWPORT vp{ 0,0,(float)width,(float)height,0,1 };
    const D3D12_RECT sc{ 0,0,(LONG)width,(LONG)height };

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    // RootSig v2:
    // 0 = b0 per-frame
    // 2 = scene table (space0)
    // 3 = deferred input table (space1)

    cmd->SetGraphicsRootConstantBufferView(0, perFrameCb);
    cmd->SetGraphicsRootDescriptorTable(2, sceneTable);
    cmd->SetGraphicsRootDescriptorTable(3, deferredInputTable);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

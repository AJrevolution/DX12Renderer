#include "Source/Renderer/Passes/GBufferPass.h"

void GBufferPass::Initialize(
    ID3D12Device* device,
    DXGI_FORMAT rt0,
    DXGI_FORMAT rt1,
    DXGI_FORMAT rt2,
    DXGI_FORMAT dsv,
    const fs::path& shaderDir)
{
    if (m_initialized) return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"GBuffer_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"GBuffer_PS.cso");

    m_rootSig.InitializeForwardPBRV2(device);
    m_pso.InitialiseGBuffer(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rt0, rt1, rt2, dsv);

    m_initialized = true;
}

void GBufferPass::Render(
    CommandList& cl,
    uint32_t width,
    uint32_t height,
    D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
    D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneTableGpu,
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

    // RootSig v2 contract:
    //  param 2 = scene table (space0)
    //  param 3 = material table (space1)
    cmd->SetGraphicsRootDescriptorTable(2, sceneTableGpu);
    cmd->SetGraphicsRootDescriptorTable(3, material.table.gpu);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &mesh.VBV());
    cmd->IASetIndexBuffer(&mesh.IBV());

    cmd->DrawIndexedInstanced(mesh.IndexCount(), 1, 0, 0, 0);
}

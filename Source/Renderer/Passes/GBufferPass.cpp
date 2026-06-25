#include "Source/Renderer/Passes/GBufferPass.h"

void GBufferPass::Initialize(
    ID3D12Device* device,
    DXGI_FORMAT rt0,
    DXGI_FORMAT rt1,
    DXGI_FORMAT rt2,
    DXGI_FORMAT rt3,
    DXGI_FORMAT dsv,
    const fs::path& shaderDir)
{
    if (m_initialized) return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"GBuffer_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"GBuffer_PS.cso");

    m_rootSig.InitializeForwardPBRV2(device);
    m_pso.InitialiseGBuffer(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rt0, rt1, rt2, rt3, dsv, D3D12_CULL_MODE_BACK, false);
    m_psoReversed.InitialiseGBuffer(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rt0, rt1, rt2, rt3, dsv, D3D12_CULL_MODE_BACK, true);
    m_psoNoCull.InitialiseGBuffer(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rt0, rt1, rt2, rt3, dsv, D3D12_CULL_MODE_NONE, false);
    m_psoNoCullReversed.InitialiseGBuffer(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rt0, rt1, rt2, rt3, dsv, D3D12_CULL_MODE_NONE, true);
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
    const Mesh& mesh,
    const Mesh::Submesh* submesh,
    bool reversesWinding)
{
    auto* cmd = cl.Get();

    const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
    const D3D12_RECT sc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

    const Mesh::Submesh& drawRange =
        submesh ? *submesh : mesh.WholeMeshSubmesh();

    if (drawRange.indexCount == 0)
        return;

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    cmd->SetGraphicsRootConstantBufferView(0, perFrameCb);
    cmd->SetGraphicsRootConstantBufferView(1, perDrawCb);
    
    const bool doubleSided =
        material.doubleSided != 0u;

    ID3D12PipelineState* selectedPso = nullptr;

    if (doubleSided)
    {
        selectedPso = reversesWinding
            ? m_psoNoCullReversed.Get()
            : m_psoNoCull.Get();
    }
    else
    {
        selectedPso = reversesWinding
            ? m_psoReversed.Get()
            : m_pso.Get();
    }

    cmd->SetPipelineState(selectedPso);

    // RootSig v2 contract:
    //  param 0 = b0 per-frame
    //  param 1 = b1 per-draw
    //  param 2 = scene table (space0), unused by shadow shaders
    //  param 3 = material table (space1), used by Shadow_FS for base-color alpha
    cmd->SetGraphicsRootDescriptorTable(2, sceneTableGpu);
    cmd->SetGraphicsRootDescriptorTable(3, material.table.gpu);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &mesh.VBV());
    cmd->IASetIndexBuffer(&mesh.IBV());

    cmd->DrawIndexedInstanced(
        drawRange.indexCount,
        1,
        drawRange.indexStart,
        drawRange.vertexBase,
        0);
}

#include "ForwardPBRPass.h"

void ForwardPBRPass::Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const fs::path& shaderDir)
{
    if (m_initialized) return;

    Shader vs = Shader::LoadFromFile(shaderDir / L"ForwardPBR_VS.cso");
    Shader ps = Shader::LoadFromFile(shaderDir / L"ForwardPBR_FS.cso");

    m_rootSig.InitializeForwardPBRV2(device);

    m_pso.InitialiseForwardPBR(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat, dsvFormat, D3D12_CULL_MODE_BACK, false);
    m_psoReversed.InitialiseForwardPBR(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat, dsvFormat, D3D12_CULL_MODE_BACK, true);
    m_psoNoCull.InitialiseForwardPBR(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat, dsvFormat, D3D12_CULL_MODE_NONE, false);
    m_psoNoCullReversed.InitialiseForwardPBR(device, m_rootSig.Get(), vs.GetBytecode(), ps.GetBytecode(), rtvFormat, dsvFormat, D3D12_CULL_MODE_NONE, true);
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

    cmd->DrawIndexedInstanced(
        drawRange.indexCount,
        1,
        drawRange.indexStart,
        drawRange.vertexBase,
        0);
}


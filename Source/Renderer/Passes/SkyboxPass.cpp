#include "SkyboxPass.h"

#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void SkyboxPass::Initialize(
    ID3D12Device* device,
    DXGI_FORMAT rtvFormat,
    DXGI_FORMAT dsvFormat,
    const fs::path& shaderDir)
{
    if (m_initialized)
        return;

    Shader vs =
        Shader::LoadFromFile(shaderDir / L"Skybox_VS.cso");

    Shader ps =
        Shader::LoadFromFile(shaderDir / L"Skybox_PS.cso");

    m_rootSig.InitializeSkybox(device);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSig.Get();
    desc.VS = vs.GetBytecode();
    desc.PS = ps.GetBytecode();

    desc.BlendState =
        CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    desc.RasterizerState =
        CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    desc.RasterizerState.CullMode =
        D3D12_CULL_MODE_NONE;

    desc.DepthStencilState =
        CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;

    desc.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS_EQUAL;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtvFormat;
    desc.DSVFormat = dsvFormat;
    desc.SampleDesc.Count = 1;

    ThrowIfFailed(
        device->CreateGraphicsPipelineState(
            &desc,
            IID_PPV_ARGS(&m_pso)),
        "CreateGraphicsPipelineState(Skybox)");

    SetD3D12ObjectName(
        m_pso.Get(),
        L"PSO: Skybox");

    m_initialized = true;
}

void SkyboxPass::Render(
    CommandList& cl,
    uint32_t width,
    uint32_t height,
    D3D12_GPU_VIRTUAL_ADDRESS skyConstants,
    D3D12_GPU_DESCRIPTOR_HANDLE skySrvTable)
{
    if (!m_initialized)
        return;

    auto* cmd = cl.Get();

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    D3D12_RECT sc{};
    sc.right = static_cast<LONG>(width);
    sc.bottom = static_cast<LONG>(height);

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    cmd->SetGraphicsRootConstantBufferView(
        0,
        skyConstants);

    cmd->SetGraphicsRootDescriptorTable(
        1,
        skySrvTable);

    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    cmd->DrawInstanced(3, 1, 0, 0);
}
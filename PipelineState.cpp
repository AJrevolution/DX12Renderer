#include "PipelineState.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void PipelineState::InitialiseTriangle(
    ID3D12Device* device,
    ID3D12RootSignature* rootSig,
    D3D12_SHADER_BYTECODE vs,
    D3D12_SHADER_BYTECODE ps,
    DXGI_FORMAT rtvFormat)
{
    const D3D12_INPUT_ELEMENT_DESC input[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig;
    pso.VS = vs;
    pso.PS = ps;

    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
	
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
	pso.SampleDesc.Quality = 0;

    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    
    pso.InputLayout = { input, _countof(input) };
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtvFormat;
    pso.SampleDesc.Count = 1;


    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)), "CreateGraphicsPipelineState");
    SetD3D12ObjectName(m_pso.Get(), L"PSO: Triangle");
}

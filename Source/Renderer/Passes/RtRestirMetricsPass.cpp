#include "RtRestirMetricsPass.h"

#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
    static std::vector<uint8_t> ReadFileBytes(
        const std::filesystem::path& path)
    {
        std::ifstream in(
            path,
            std::ios::binary);

        if (!in)
        {
            throw std::runtime_error(
                "Failed to open RT ReSTIR metrics shader.");
        }

        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }
}

void RtRestirMetricsPass::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);

    BuildPipelineState(
        device,
        shaderDir / L"RtRestirMetrics_CS.cso");
}

void RtRestirMetricsPass::BuildRootSignature(
    ID3D12Device* device)
{
    // t0 = measured diffuse signal
    // t1 = measured specular signal
    // t2 = optional packed ReSTIR reservoir
    // t3 = optional rejection-reason texture
    // t4 = optional diffuse albedo for final-signal remodulation
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        5,
        0,
        0);

    // u0 = RtRestirMetricPartial structured buffer
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        1,
        0,
        0);

    CD3DX12_ROOT_PARAMETER params[3]{};

    // Root parameter 0: b0 = RtRestirMetricsConstants
    params[0].InitAsConstantBufferView(
        0,
        0);

    // Root parameter 1: t0..t4
    params[1].InitAsDescriptorTable(
        1,
        &srvRange);

    // Root parameter 2: u0
    params[2].InitAsDescriptorTable(
        1,
        &uavRange);

    CD3DX12_ROOT_SIGNATURE_DESC desc{};
    desc.Init(
        _countof(params),
        params,
        0,
        nullptr);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;

    const HRESULT serializeResult =
        D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &blob,
            &errorBlob);

    if (FAILED(serializeResult) &&
        errorBlob &&
        errorBlob->GetBufferPointer())
    {
        OutputDebugStringA(
            static_cast<const char*>(
                errorBlob->GetBufferPointer()));
    }

    ThrowIfFailed(
        serializeResult,
        "Serialize RT ReSTIR metrics root signature");

    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)),
        "Create RT ReSTIR metrics root signature");

    SetD3D12ObjectName(
        m_rootSignature.Get(),
        L"RootSig: RT ReSTIR Metrics");
}

void RtRestirMetricsPass::BuildPipelineState(
    ID3D12Device* device,
    const std::filesystem::path& shaderPath)
{
    const std::vector<uint8_t> shaderBytes =
        ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature =
        m_rootSignature.Get();

    desc.CS.pShaderBytecode =
        shaderBytes.data();

    desc.CS.BytecodeLength =
        shaderBytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(
            &desc,
            IID_PPV_ARGS(&m_pipelineState)),
        "Create RT ReSTIR metrics PSO");

    SetD3D12ObjectName(
        m_pipelineState.Get(),
        L"PSO: RT ReSTIR Metrics");
}

void RtRestirMetricsPass::Dispatch(
    CommandList& commandList,
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
    D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
    uint32_t width,
    uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    ID3D12GraphicsCommandList* commandListPtr =
        commandList.Get();

    commandListPtr->SetPipelineState(
        m_pipelineState.Get());

    commandListPtr->SetComputeRootSignature(
        m_rootSignature.Get());

    // Root parameter 0: b0 = RtRestirMetricsConstants
    commandListPtr->SetComputeRootConstantBufferView(
        0,
        constants);

    // Root parameter 1: t0..t4
    commandListPtr->SetComputeRootDescriptorTable(
        1,
        inputSrvTable);

    // Root parameter 2: u0
    commandListPtr->SetComputeRootDescriptorTable(
        2,
        outputUavTable);

    constexpr uint32_t threadGroupSizeX = 16u;
    constexpr uint32_t threadGroupSizeY = 16u;

    const uint32_t groupCountX =
        (width + threadGroupSizeX - 1u) /
        threadGroupSizeX;

    const uint32_t groupCountY =
        (height + threadGroupSizeY - 1u) /
        threadGroupSizeY;

    commandListPtr->Dispatch(
        groupCountX,
        groupCountY,
        1);
}

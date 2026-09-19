#include "RtRestirComposeCurrentPass.h"
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
        std::ifstream in(path, std::ios::binary);

        if (!in)
        {
            throw std::runtime_error(
                "Failed to open RT ReSTIR compose-current shader.");
        }

        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }
}

void RtRestirComposeCurrentPass::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);

    BuildPipelineState(
        device,
        shaderDir / L"RtRestirComposeCurrent_CS.cso");
}

void RtRestirComposeCurrentPass::BuildRootSignature(
    ID3D12Device* device)
{
    // t0..t6:
    // base diffuse/specular,
    // ReSTIR diffuse/specular,
    // ReSTIR receiver normal/raw material roughness,
    // validation reference diffuse/specular.
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        7,
        0,
        0);

    // u0..u2:
    // composed diffuse,
    // composed specular,
    // debug output.
    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        3,
        0,
        0);

    CD3DX12_ROOT_PARAMETER params[3]{};

    // b0 = RtRestirComposeCurrentConstants
    params[0].InitAsConstantBufferView(0);

    // t0..t6
    params[1].InitAsDescriptorTable(
        1,
        &srvRange);

    // u0..u2
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
        "Serialize RT ReSTIR compose-current root signature");

    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)),
        "Create RT ReSTIR compose-current root signature");

    SetD3D12ObjectName(
        m_rootSignature.Get(),
        L"RootSig: RT ReSTIR Compose Current");
}

void RtRestirComposeCurrentPass::BuildPipelineState(
    ID3D12Device* device,
    const std::filesystem::path& shaderPath)
{
    const std::vector<uint8_t> shaderBytes =
        ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSignature.Get();
    desc.CS.pShaderBytecode = shaderBytes.data();
    desc.CS.BytecodeLength = shaderBytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(
            &desc,
            IID_PPV_ARGS(&m_pipelineState)),
        "Create RT ReSTIR compose-current PSO");

    SetD3D12ObjectName(
        m_pipelineState.Get(),
        L"PSO: RT ReSTIR Compose Current");
}

void RtRestirComposeCurrentPass::Dispatch(
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

    // Root parameter 0: b0
    commandListPtr->SetComputeRootConstantBufferView(
        0,
        constants);

    // Root parameter 1: t0..t6
    commandListPtr->SetComputeRootDescriptorTable(
        1,
        inputSrvTable);

    // Root parameter 2: u0..u2
    commandListPtr->SetComputeRootDescriptorTable(
        2,
        outputUavTable);

    constexpr uint32_t threadGroupSizeX = 8;
    constexpr uint32_t threadGroupSizeY = 8;

    const uint32_t groupCountX =
        (width + threadGroupSizeX - 1) /
        threadGroupSizeX;

    const uint32_t groupCountY =
        (height + threadGroupSizeY - 1) /
        threadGroupSizeY;

    commandListPtr->Dispatch(
        groupCountX,
        groupCountY,
        1);
}

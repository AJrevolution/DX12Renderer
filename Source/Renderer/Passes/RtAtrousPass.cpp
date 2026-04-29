#include "RtAtrousPass.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"
#include <fstream>

namespace
{
    static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Failed to open RT A-Trous shader.");
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
}

void RtAtrousPass::Initialize(ID3D12Device* device, const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);
    BuildPipelineState(device, shaderDir / L"RtAtrous_CS.cso");
}

void RtAtrousPass::BuildRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0); // t0..t3

    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0); // u0 output

    CD3DX12_ROOT_PARAMETER params[3]{};
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &srvRange);
    params[2].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_STATIC_SAMPLER_DESC clampSampler(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC desc{};
    desc.Init(_countof(params), params, 1, &clampSampler);

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
        "Serialize RT A-Trous root sig");
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)),
        "Create RT A-Trous root sig");

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: RT A-Trous");
}

void RtAtrousPass::BuildPipelineState(ID3D12Device* device, const std::filesystem::path& shaderPath)
{
    const auto bytes = ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSig.Get();
    desc.CS.pShaderBytecode = bytes.data();
    desc.CS.BytecodeLength = bytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pso)),
        "Create RT A-Trous PSO");

    SetD3D12ObjectName(m_pso.Get(), L"PSO: RT A-Trous");
}

void RtAtrousPass::Dispatch(
    CommandList& cl,
    D3D12_GPU_VIRTUAL_ADDRESS constantsCb,
    D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
    D3D12_GPU_DESCRIPTOR_HANDLE outputUav,
    uint32_t width,
    uint32_t height)
{
    auto* cmd = cl.Get();

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRootConstantBufferView(0, constantsCb);
    cmd->SetComputeRootDescriptorTable(1, inputSrvTable);
    cmd->SetComputeRootDescriptorTable(2, outputUav);

    const uint32_t gx = (width + 7u) / 8u;
    const uint32_t gy = (height + 7u) / 8u;
    cmd->Dispatch(gx, gy, 1);
}
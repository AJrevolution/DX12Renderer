#include "RtCombinePass.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"
#include <fstream>

namespace
{
    static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Failed to open RT combine shader.");
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
}

void RtCombinePass::Initialize(ID3D12Device* device, const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);
    BuildPipelineState(device, shaderDir / L"RtCombine_CS.cso");
}

void RtCombinePass::BuildRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0); // t0 diffuse, t1 spec

    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0); // u0 output

    CD3DX12_ROOT_PARAMETER params[2]{};
    params[0].InitAsDescriptorTable(1, &srvRange);
    params[1].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_ROOT_SIGNATURE_DESC desc{};
    desc.Init(_countof(params), params, 0, nullptr);

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err),
        "Serialize RT combine root sig");

    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)),
        "Create RT combine root sig");

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: RT Combine");
}

void RtCombinePass::BuildPipelineState(ID3D12Device* device, const std::filesystem::path& shaderPath)
{
    const auto bytes = ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSig.Get();
    desc.CS.pShaderBytecode = bytes.data();
    desc.CS.BytecodeLength = bytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pso)),
        "Create RT combine PSO");

    SetD3D12ObjectName(m_pso.Get(), L"PSO: RT Combine");
}

void RtCombinePass::Dispatch(
    CommandList& cl,
    D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
    D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
    uint32_t width,
    uint32_t height)
{
    auto* cmd = cl.Get();

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRootDescriptorTable(0, inputSrvTable);
    cmd->SetComputeRootDescriptorTable(1, outputUavTable);

    const uint32_t gx = (width + 7u) / 8u;
    const uint32_t gy = (height + 7u) / 8u;
    cmd->Dispatch(gx, gy, 1);
}

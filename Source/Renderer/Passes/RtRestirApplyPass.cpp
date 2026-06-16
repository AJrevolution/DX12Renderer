#include "RtRestirApplyPass.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

#include <fstream>
#include <vector>
#include <iterator>
#include <stdexcept>

namespace
{
    static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Failed to open RT ReSTIR apply shader.");

        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    }
}

void RtRestirApplyPass::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);
    BuildPipelineState(device, shaderDir / L"RtRestirApply_CS.cso");
}

void RtRestirApplyPass::BuildRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0); // t0..t3

    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0); // u0..u1

    CD3DX12_ROOT_PARAMETER params[3]{};

    // b0 = RtRestirApplyConstants
    params[0].InitAsConstantBufferView(0);

    // t0..t3 = base diffuse/spec + ReSTIR diffuse/spec
    params[1].InitAsDescriptorTable(1, &srvRange);

    // u0..u1 = output diffuse/spec
    params[2].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_ROOT_SIGNATURE_DESC desc{};
    desc.Init(_countof(params), params, 0, nullptr);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;

    ThrowIfFailed(
        D3D12SerializeRootSignature(
            &desc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &blob,
            &err),
        "Serialize RT ReSTIR apply root sig");

    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)),
        "Create RT ReSTIR apply root sig");

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: RT ReSTIR Apply");
}

void RtRestirApplyPass::BuildPipelineState(
    ID3D12Device* device,
    const std::filesystem::path& shaderPath)
{
    const auto bytes = ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSig.Get();
    desc.CS.pShaderBytecode = bytes.data();
    desc.CS.BytecodeLength = bytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(
            &desc,
            IID_PPV_ARGS(&m_pso)),
        "Create RT ReSTIR apply PSO");

    SetD3D12ObjectName(m_pso.Get(), L"PSO: RT ReSTIR Apply");
}

void RtRestirApplyPass::Dispatch(
    CommandList& cl,
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
    D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
    uint32_t width,
    uint32_t height)
{
    auto* cmd = cl.Get();

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootSignature(m_rootSig.Get());

    cmd->SetComputeRootConstantBufferView(0, constants);
    cmd->SetComputeRootDescriptorTable(1, inputSrvTable);
    cmd->SetComputeRootDescriptorTable(2, outputUavTable);

    const uint32_t gx = (width + 7u) / 8u;
    const uint32_t gy = (height + 7u) / 8u;

    cmd->Dispatch(gx, gy, 1);
}

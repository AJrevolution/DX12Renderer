#include "RtHitDistReconstructPass.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
    static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Failed to open RT hit-distance reconstruct shader.");

        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }
}

void RtHitDistReconstructPass::Initialize(
    ID3D12Device* device,
    const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);
    BuildPipelineState(device, shaderDir / L"RtHitDistReconstruct_CS.cso");
}

void RtHitDistReconstructPass::BuildRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0);
    // t0 raw ViewZ-compatible distance
    // t1 current normal/roughness
    // t2 current depth
    // t3 raw prevUV
    // t4 previous reconstructed hit distance
    // t5 previous depth
    // t6 previous normal/roughness
    // t7 surface id

    CD3DX12_DESCRIPTOR_RANGE uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0);
    // u0 reconstructed hit distance
    // u1 reconstructed confidence
    // u2 debug/display output

    CD3DX12_ROOT_PARAMETER params[3]{};
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsDescriptorTable(1, &srvRange);
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
        "Serialize RT hit-distance reconstruct root sig");

    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            blob->GetBufferPointer(),
            blob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSig)),
        "Create RT hit-distance reconstruct root sig");

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: RT Hit Distance Reconstruct");
}

void RtHitDistReconstructPass::BuildPipelineState(
    ID3D12Device* device,
    const std::filesystem::path& shaderPath)
{
    const auto bytes = ReadFileBytes(shaderPath);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSig.Get();
    desc.CS.pShaderBytecode = bytes.data();
    desc.CS.BytecodeLength = bytes.size();

    ThrowIfFailed(
        device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pso)),
        "Create RT hit-distance reconstruct PSO");

    SetD3D12ObjectName(m_pso.Get(), L"PSO: RT Hit Distance Reconstruct");
}

void RtHitDistReconstructPass::Dispatch(
    CommandList& cl,
    D3D12_GPU_VIRTUAL_ADDRESS constantsCb,
    D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
    D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
    uint32_t width,
    uint32_t height)
{
    auto* cmd = cl.Get();

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetComputeRootSignature(m_rootSig.Get());

    cmd->SetComputeRootConstantBufferView(0, constantsCb);
    cmd->SetComputeRootDescriptorTable(1, inputSrvTable);
    cmd->SetComputeRootDescriptorTable(2, outputUavTable);

    const uint32_t gx = (width + 7u) / 8u;
    const uint32_t gy = (height + 7u) / 8u;
    cmd->Dispatch(gx, gy, 1);
}

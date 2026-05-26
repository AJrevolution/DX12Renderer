#pragma once

#include <filesystem>
#include "Common.h"
#include "Source/RHI/CommandList/CommandList.h"
#include <DirectXMath.h>
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

class RtMotionDilatePass
{
public:
    void Initialize(ID3D12Device* device, const std::filesystem::path& shaderDir);

    void Dispatch(
        CommandList& cl,
        D3D12_GPU_VIRTUAL_ADDRESS constantsCb,
        D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
        D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
        uint32_t width,
        uint32_t height);

private:

    void BuildRootSignature(ID3D12Device* device);
    void BuildPipelineState(ID3D12Device* device, const std::filesystem::path& shaderPath);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
};

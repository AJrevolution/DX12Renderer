#pragma once

#include <filesystem>
#include "Common.h"
#include "Source/RHI/CommandList/CommandList.h"

class RtRestirComposeCurrentPass
{
public:
    void Initialize(
        ID3D12Device* device,
        const std::filesystem::path& shaderDir);

    void Dispatch(
        CommandList& commandList,
        D3D12_GPU_VIRTUAL_ADDRESS constants,
        D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
        D3D12_GPU_DESCRIPTOR_HANDLE outputUavTable,
        uint32_t width,
        uint32_t height);

private:
    void BuildRootSignature(
        ID3D12Device* device);

    void BuildPipelineState(
        ID3D12Device* device,
        const std::filesystem::path& shaderPath);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
};

#pragma once

#include "Common.h"
#include "Source/RHI/CommandList/CommandList.h"
#include "Source/RHI/Pipeline/RootSignature.h"
#include "PipelineState.h"
#include "Source/RHI/Pipeline/Shader.h"

#include <filesystem>

namespace fs = std::filesystem;

class SkyboxPass
{
public:
    void Initialize(
        ID3D12Device* device,
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT dsvFormat,
        const fs::path& shaderDir);

    void Render(
        CommandList& cl,
        uint32_t width,
        uint32_t height,
        D3D12_GPU_VIRTUAL_ADDRESS skyConstants,
        D3D12_GPU_DESCRIPTOR_HANDLE skySrvTable);

private:
    bool m_initialized = false;

    RootSignature m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
};
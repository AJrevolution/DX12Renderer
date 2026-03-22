#pragma once
#include "Common.h"
#include "Source/RHI/Pipeline/Shader.h"
#include "Source/RHI/Pipeline/RootSignature.h"
#include "PipelineState.h"
#include "Source/RHI/CommandList/CommandList.h"

#include <filesystem>
namespace fs = std::filesystem;

class DeferredLightingPass
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, const fs::path& shaderDir);

    void Render(
        CommandList& cl,
        uint32_t width,
        uint32_t height,
        D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneTable);

private:
    bool m_initialized = false;
    RootSignature m_rootSig;
    PipelineState m_pso;
};

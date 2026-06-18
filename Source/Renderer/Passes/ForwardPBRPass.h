#pragma once
#include "Common.h"
#include "Source/RHI/Pipeline/Shader.h"
#include "Source/RHI/Pipeline/RootSignature.h"
#include "PipelineState.h"
#include "Source/RHI/CommandList/CommandList.h"
#include "Source/Scene/Mesh.h"
#include "Source/Scene/Material.h"

#include <filesystem>
namespace fs = std::filesystem;

class ForwardPBRPass
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat, const fs::path& shaderDir);

    void Render(
        CommandList& cl,
        uint32_t width,
        uint32_t height,
        D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,
        D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneTable, 
        const Material& material,
        const Mesh& mesh,
        const Mesh::Submesh* submesh = nullptr);

private:
    bool m_initialized = false;
    RootSignature m_rootSig;
    PipelineState m_pso;
};

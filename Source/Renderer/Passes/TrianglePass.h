#pragma once
#include "Common.h"
#include "Source\RHI\Pipeline\Shader.h"
#include "Source\RHI\Pipeline\RootSignature.h"
#include "PipelineState.h"
#include "Source/RHI/Resources/GPUBuffer.h"
#include "Source/RHI/Memory/UploadArena.h"

#include <filesystem>
namespace fs = std::filesystem;

class TrianglePass
{
public:
    void Initialize(
        ID3D12Device* device, 
        DXGI_FORMAT rtvFormat, 
        const fs::path& shaderDir,
        ID3D12GraphicsCommandList* cmd,
        UploadArena& upload,
        uint32_t frameIndex);

    void Render(
        ID3D12GraphicsCommandList* cmd,
        uint32_t width,
        uint32_t height,
        D3D12_GPU_VIRTUAL_ADDRESS globalCB,     //Root Param 1 (register b0)
		D3D12_GPU_DESCRIPTOR_HANDLE textureSRV  //Root Param 2 (Descriptor Table)
    );
    
    bool IsInitialized() const { return m_initialized; }

private:
    struct Vertex
    {
        float px, py, pz;
        float r, g, b, a;
        float u, v;
    };

    bool m_initialized = false;

    RootSignature m_rootSig;
    PipelineState m_pso;

    GPUBuffer m_vbDefault;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
};

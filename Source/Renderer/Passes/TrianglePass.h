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
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        uint32_t width,
        uint32_t height
    );
    
    bool IsInitialized() const { return m_initialized; }

private:
    struct Vertex
    {
        float px, py, pz;
        float r, g, b, a;
    };

    bool m_initialized = false;

    RootSignature m_rootSig;
    PipelineState m_pso;

    GPUBuffer m_vbDefault;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
};

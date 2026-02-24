#pragma once
#include "Common.h"
#include "Source\RHI\Pipeline\Shader.h"
#include "Source\RHI\Pipeline\RootSignature.h"
#include "PipelineState.h"
#include "Source/RHI/Resources/GPUBuffer.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/Scene/Mesh.h"
#include "Source/Scene/Material.h"

#include <filesystem>
namespace fs = std::filesystem;

class TrianglePass
{
public:
    void Initialize(
        ID3D12Device* device, 
        DXGI_FORMAT rtvFormat, 
        const fs::path& shaderDir   );

    void Render(
        ID3D12GraphicsCommandList* cmd,
        uint32_t width,
        uint32_t height,
        D3D12_GPU_VIRTUAL_ADDRESS perFrameCb,   // root param 0 (b0)
        D3D12_GPU_VIRTUAL_ADDRESS perDrawCb,    // root param 1 (b1)
        const Material& material,
        const Mesh& mesh
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

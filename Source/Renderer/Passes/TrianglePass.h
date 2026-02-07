#pragma once
#include "Common.h"
#include "Source\RHI\Pipeline\Shader.h"
#include "Source\RHI\Pipeline\RootSignature.h"
#include "PipelineState.h"

#include <filesystem>
namespace fs = std::filesystem;

class TrianglePass
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT rtvFormat, const fs::path& shaderDir);

    void Render(
        ID3D12GraphicsCommandList* cmd,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        uint32_t width,
        uint32_t height
    );

private:
    struct Vertex
    {
        float px, py, pz;
        float r, g, b, a;
    };

    RootSignature m_rootSig;
    PipelineState m_pso;

    ComPtr<ID3D12Resource> m_vb;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
};

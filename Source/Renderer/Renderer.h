#pragma once
#include "Common.h"
#include "Source/Renderer/Passes/TrianglePass.h"
//#include "Source/RHI/Diagnostics/GPUTimerSet.h"
#include "GPUMarkers.h"
#include "Source/RHI/Memory/UploadArena.h"
#include <filesystem>

class Renderer
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount);

    void BeginFrame(uint32_t frameIndex);

    void RenderFrame(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmd,
        uint32_t frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        uint32_t width,
        uint32_t height
    );

private:
    TrianglePass m_triangle;
    UploadArena  m_upload;
    DXGI_FORMAT  m_backbufferFormat = DXGI_FORMAT_UNKNOWN;
    bool         m_triangleReady = false;
};

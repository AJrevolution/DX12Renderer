#pragma once
#include "Common.h"
#include "Source/Renderer/Passes/TrianglePass.h"
#include "Source/RHI/Diagnostics/GPUTimerSet.h"
#include "GPUMarkers.h"
#include <filesystem>

class Renderer
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat);

    void RenderFrame(
        ID3D12GraphicsCommandList* cmd,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        uint32_t width,
        uint32_t height
    );

private:
    TrianglePass m_triangle;
};

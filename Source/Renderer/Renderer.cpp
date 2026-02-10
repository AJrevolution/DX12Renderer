#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat)
{
    m_triangle.Initialize(device, backbufferFormat, Paths::ShaderDir());
}

void Renderer::RenderFrame(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv, uint32_t width, uint32_t height)
{
    CmdBeginEvent(cmd, "Renderer");

    CmdBeginEvent(cmd, "TrianglePass");
    m_triangle.Render(cmd, backbufferRtv, width, height);
    CmdEndEvent(cmd);

    CmdEndEvent(cmd);
}

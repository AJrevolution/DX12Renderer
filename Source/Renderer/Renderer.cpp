#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/RHI/Memory/UploadArena.h"

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    //1 MB per frame for now, increase later for textures.
    m_upload.Initialize(device, frameCount, 1ull * 1024ull * 1024ull);
}

void Renderer::RenderFrame(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmd,
    uint32_t frameIndex,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    uint32_t width,
    uint32_t height)
{
    CmdBeginEvent(cmd, "Renderer");
    
    //Reset per-frame upload head (safe because Application already waited this frame index fence)
    m_upload.BeginFrame(frameIndex);
    
    //Lazy init - record copy into DEFAULT VB on the current command list once
    if (!m_triangleReady)
    {
        m_triangle.Initialize(
            device,
            m_backbufferFormat,
            Paths::ShaderDir(),
            cmd,
            m_upload,
            frameIndex
        );
        m_triangleReady = true;
    }

    CmdBeginEvent(cmd, "TrianglePass");
    m_triangle.Render(cmd, backbufferRtv, width, height);
    CmdEndEvent(cmd);

    CmdEndEvent(cmd);
}

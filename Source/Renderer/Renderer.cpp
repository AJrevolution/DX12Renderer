#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    //1 MB per frame for now, increase later for textures.
    m_upload.Initialize(device, frameCount, 1ull * 1024ull * 1024ull);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
}

void Renderer::BeginFrame(uint32_t frameIndex)
{
    // FrameIndex is already fence-safe by the time Application calls this.
    m_upload.BeginFrame(frameIndex);
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

    if (!m_depthReady)
    {
        DebugOutput("Renderer::RenderFrame: depth not initialized! Skipping depth operations.");
        // If depth isn't ready, bind the RTV solo to avoid crashing
        cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    }
    else
    {
        // Clear and Bind
        cmd->ClearDepthStencilView(m_depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Bind BOTH the RTV and the DSV
        cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, &m_depthDsv);
    }

    CmdBeginEvent(cmd, "TrianglePass");
    m_triangle.Render(cmd, backbufferRtv, width, height);
    CmdEndEvent(cmd);


    CmdEndEvent(cmd);
}

void Renderer::OnResize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    // Recreate depth on resize
    m_depth.CreateDepth(device, width, height, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, L"Depth Buffer");

    // Allocate the DSV once; reuse handle across recreations
    if (m_depthDsv.ptr == 0)
        m_depthDsv = m_dsvHeap.Allocate(1).cpu;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    device->CreateDepthStencilView(m_depth.Get(), &dsv, m_depthDsv);

    m_depthReady = true;
}

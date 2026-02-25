#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "DrawConstants.h"

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    //1 MB per frame for now, increase later for textures.
    m_upload.Initialize(device, frameCount, 16 * 1024 * 1024);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
    
    // Shader-visible SRV heap for textures
    m_srvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, true, L"SRV Heap (Shader Visible)");

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

    //Prepare global data
    //D3D12_GPU_VIRTUAL_ADDRESS globalCB = UpdateGlobalConstants(frameIndex, width, height);

    if (!m_resourcesReady)
    {
        SetupResources(device, cmd, frameIndex);
        m_resourcesReady = true;
    }
    
    // Depth must already be created by OnResize
    if (m_depthReady)
    {
        cmd->ClearDepthStencilView(m_depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, &m_depthDsv);
    }
    else
    {
        cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    }

    //Bind the heap before drawing
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);

    // Per-frame CB (b0)
    const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb = UpdateGlobalConstants(frameIndex, width, height);

    // Per-draw CB (b1) allocated from UploadArena, 256-aligned
    {
        auto drawAlloc = m_upload.Allocate(frameIndex, sizeof(PerDrawConstants), 256);
        auto* dc = reinterpret_cast<PerDrawConstants*>(drawAlloc.cpu);

        using namespace DirectX;
        const float t = 0.0f; // replace with timer later
        XMStoreFloat4x4(&dc->world, XMMatrixIdentity()); // or rotation: XMMatrixRotationZ(t)

        dc->materialIndex = 0;

        CmdBeginEvent(cmd, "TexturedQuadPass");
        m_triangle.Render(cmd, width, height, perFrameCb, drawAlloc.gpu, m_material, m_quad);
        CmdEndEvent(cmd);
    }

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

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateGlobalConstants(uint32_t frameIndex, uint32_t width, uint32_t height)
{
    // The 256 alignment is handled by the allocator, but we ensure the size is also a multiple of 256
    constexpr uint32_t cbSize = (sizeof(PerFrameConstants) + 255) & ~255;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<PerFrameConstants*>(alloc.cpu);

    // Fill the data
    DirectX::XMStoreFloat4x4(&cb->viewProj, DirectX::XMMatrixIdentity());
    cb->cameraPos = { 0.0f, 0.0f, 0.0f };
    cb->time = 0.0f; // Tie this to a timer later
    cb->frameIndex = frameIndex;

    return alloc.gpu;
}


void Renderer::SetupResources(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
    // Initialize the PASS (PSO + Root Sig) TODO sort out triangle intialising
    m_triangle.Initialize(device, m_backbufferFormat, Paths::ShaderDir());

    // Create mesh GPU resources (DEFAULT heap VB+IB)
    m_quad.CreateTexturedQuad(device, cmd, m_upload, frameIndex);

    // Load a real texture from disk 
    const auto content = Paths::ContentDir_DevOnly();
    if (!content.empty())
    {
        m_material.baseColorSrv = m_albedoTex.LoadFromFile_DirectXTex(
            device, cmd, m_upload, frameIndex,
            content / L"Textures" / L"checker.png",
            m_srvHeap,
            true,
            L"Tex: Albedo");
        m_sceneReady = true;
    }
    else
    {
        DebugOutput("Renderer::SetupResources: ContentDir not found.");

        m_sceneReady = true; // false if want to hard-fail
    }
}
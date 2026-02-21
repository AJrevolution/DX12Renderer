#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    //1 MB per frame for now, increase later for textures.
    m_upload.Initialize(device, frameCount, 1ull * 1024ull * 1024ull);

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
    D3D12_GPU_VIRTUAL_ADDRESS globalCB = UpdateGlobalConstants(frameIndex, width, height);

    if (!m_resourcesReady)
    {
        SetupResources(device, cmd, frameIndex);
        m_resourcesReady = true;
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

    //Bind the heap before drawing
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmd, "TrianglePass");
    m_triangle.Render(cmd, width, height, globalCB, m_testTextureSrv.gpu);
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

void Renderer::CreateTestTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
    m_testTexture.Create2D(device, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, L"Checkerboard Texture");

    uint32_t pixels[] = {
        0xFFFFFFFF, 0xFF000000,
        0xFF000000, 0xFFFFFFFF
    };
    
    const uint64_t rowPitch = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    const uint64_t totalSize = rowPitch * 2;

    auto upload = m_upload.Allocate(frameIndex, totalSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    uint8_t* pDest = reinterpret_cast<uint8_t*>(upload.cpu);
    memcpy(pDest, &pixels[0], 8);           // Row 0 (2 pixels * 4 bytes)
    memcpy(pDest + rowPitch, &pixels[2], 8); // Row 1

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_upload.GetBuffer(frameIndex);
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    // THIS OFFSET MUST BE A MULTIPLE OF 512
    src.PlacedFootprint.Offset = upload.offset;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 2;
    src.PlacedFootprint.Footprint.Height = 2;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = (UINT)rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_testTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_testTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    cmd->ResourceBarrier(1, &barrier);

    m_testTextureSrv = m_srvHeap.Allocate(1);
    device->CreateShaderResourceView(m_testTexture.Get(), nullptr, m_testTextureSrv.cpu);
}

void Renderer::SetupResources(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
    // Initialize Triangle Pass (records VB upload to current frameIndex)
    m_triangle.Initialize(device, m_backbufferFormat, Paths::ShaderDir(), cmd, m_upload, frameIndex);

    // Create and Upload Texture (records Texture upload to current frameIndex)
    CreateTestTexture(device, cmd, frameIndex);
}
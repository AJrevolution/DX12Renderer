#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "DrawConstants.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"


void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    //1 MB per frame for now, increase later for textures.
    m_upload.Initialize(device, frameCount, 16 * 1024 * 1024);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
    
    // Shader-visible SRV heap for textures
    m_srvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, true, L"SRV Heap (Shader Visible)");

    // Space 0: Scene Table (t0..t3)
    static constexpr uint32_t kSceneSrvCount = 4;
    //m_sceneTable = m_srvHeap.Allocate(kSceneSrvCount);
    CreateNullSceneTable(device);

    //D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    //nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    //nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    //nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    //nullSrv.Texture2D.MipLevels = 1;

    //for (uint32_t i = 0; i < kSceneSrvCount; ++i)
    //{
    //    // Manual offset: Base + (index * size)
    //    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_sceneTable.cpu;
    //    handle.ptr += (SIZE_T)i * m_srvHeap.DescriptorSize();
    //
    //    device->CreateShaderResourceView(nullptr, &nullSrv, handle);
    //}

}

void Renderer::BeginFrame(uint32_t frameIndex)
{
    // FrameIndex is already fence-safe by the time Application calls this.
    m_upload.BeginFrame(frameIndex);
}

void Renderer::RenderFrame(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    ID3D12Resource* pBackBuffer,
    uint32_t width,
    uint32_t height,
    float time)
{
    auto* cmdList = cl.Get();

    CmdBeginEvent(cmdList, "Renderer");

    //Prepare global data

    if (m_depthReady)
    {
        cl.Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    if (!m_resourcesReady)
    {
        SetupResources(device, cl, frameIndex);
        m_resourcesReady = true;
    }
    cl.FlushBarriers();
    // Depth must already be created by OnResize
    cmdList->ClearRenderTargetView(backbufferRtv, m_clearColor, 0, nullptr);
    if (m_depthReady)
    {
        cmdList->ClearDepthStencilView(m_depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &backbufferRtv, FALSE, &m_depthDsv);
    }
    else
    {
        cmdList->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    }

    //Bind the heap before drawing
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Per-frame CB (b0)
    const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb = UpdateGlobalConstants(frameIndex, width, height, time);

    // Per-draw CB (b1) allocated from UploadArena, 256-aligned
    {
        // Per-Draw CB using PBR factors from the material
        auto drawAlloc = m_upload.Allocate(frameIndex, sizeof(PerDrawConstants), 256);
        auto* dc = reinterpret_cast<PerDrawConstants*>(drawAlloc.cpu);
        
        //temp to observe lighting calulations
        float oscillation = sinf(time * 1.5f);
        float rotationAngle = oscillation * 1.1f;

        DirectX::XMMATRIX world = DirectX::XMMatrixRotationY(rotationAngle);
        DirectX::XMStoreFloat4x4(&dc->world, world);
        dc->materialIndex = 0;
        dc->baseColorFactor = m_material.baseColorFactor;
        dc->metallicFactor = m_material.metallicFactor;
        dc->roughnessFactor = m_material.roughnessFactor;
        const float t = 0.0f; // TODO replace with timer later

        CmdBeginEvent(cmdList, "TexturedQuadPass");
        m_forwardPbr.Render(
            cl, width, height,
            perFrameCb,     // b0
            drawAlloc.gpu,  // b1
            m_scene.table.gpu, // Space 0
            m_material,     // Space 1
            m_quad
        );
        CmdEndEvent(cmdList);
    }

    CmdEndEvent(cmdList);
}

void Renderer::OnResize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    // Recreate depth on resize
    m_depth.CreateDepth(device, width, height, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, L"Depth Buffer");
    CommandList::SetGlobalState(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Allocate the DSV once; reuse handle across recreations
    if (m_depthDsv.ptr == 0)
        m_depthDsv = m_dsvHeap.Allocate(1).cpu;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    device->CreateDepthStencilView(m_depth.Get(), &dsv, m_depthDsv);

    m_depthReady = true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateGlobalConstants(uint32_t frameIndex, uint32_t width, uint32_t height, float time)
{
    // The 256 alignment is handled by the allocator, but we ensure the size is also a multiple of 256
    constexpr uint32_t cbSize = (sizeof(PerFrameConstants) + 255) & ~255;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<PerFrameConstants*>(alloc.cpu);

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    
    const auto& cam = m_sceneData.camera;
    const auto& sun = m_sceneData.sun;

    DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(
        XMLoadFloat3(&cam.position),
        XMLoadFloat3(&cam.target),
        XMVectorSet(0, 1, 0, 0));

    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(cam.fovY, aspect, cam.nearZ, cam.farZ);
    DirectX::XMMATRIX viewProj = view * proj;

    DirectX::XMStoreFloat4x4(&cb->viewProj, viewProj);

    cb->cameraPos = cam.position;
    cb->time = time; 
    cb->frameIndex = frameIndex;
    cb->padding[0] = cb->padding[1] = cb->padding[2] = 0.0f;

    //light
    cb->lightDir = sun.direction;
    cb->pad1 = 0.0f;
    cb->lightColor = {
        sun.color.x * sun.intensity,
        sun.color.y * sun.intensity,
        sun.color.z * sun.intensity
    };
    cb->pad2 = 0.0f;

    return alloc.gpu;
}


void Renderer::SetupResources(ID3D12Device* device, CommandList& cl, uint32_t frameIndex)
{
    m_forwardPbr.Initialize(device, m_backbufferFormat, DXGI_FORMAT_D32_FLOAT, Paths::ShaderDir());

    // Create mesh GPU resources (DEFAULT heap VB+IB)
    m_quad.CreateTexturedQuad(device, cl, m_upload, frameIndex);

    // Load a real texture from disk 
    const auto content = Paths::ContentDir_DevOnly();
    if (!content.empty())
    {
        m_albedoTex.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex,
            content / L"Textures" / L"checker.png",
             true, L"Tex: Albedo");

        m_normalTex.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex,
            content / L"Textures" / L"checker_normal_map.png",
             false, L"Tex: Normal");

        m_metalRoughTex.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex,
            content / L"Textures" / L"checker_ORM_map.png",
            false,
            L"Tex: MetalRough");

        // Material handles the table allocation and SRV placement
        m_material.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);

        // Fill in PBR factors
        m_material.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_material.metallicFactor = 0.5f;
        m_material.roughnessFactor = 0.3f;
    }
    else
    {
        DebugOutput("Renderer::SetupResources: ContentDir not found.");

        m_sceneReady = true; // false if want to hard-fail
    }
}

void Renderer::CreateNullSceneTable(ID3D12Device* device)
{
    if (!m_scene.IsValid())
        m_scene.table = m_srvHeap.Allocate(SceneResources::COUNT);

    for (uint32_t i = 0; i < SceneResources::COUNT; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_scene.table.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        CreateNullTexture2DSRV(device, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
}

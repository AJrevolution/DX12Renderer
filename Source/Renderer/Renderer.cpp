#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "DrawConstants.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"


void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    m_upload.Initialize(device, frameCount, 64 * 1024 * 1024);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
    
    // Shader-visible SRV heap for textures
    m_srvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, true, L"SRV Heap (Shader Visible)");

    // Space 0: Scene Table (t0..t3)
    static constexpr uint32_t kSceneSrvCount = 4;
    //m_sceneTable = m_srvHeap.Allocate(kSceneSrvCount);
    CreateNullSceneTable(device);
    
    CreateNullDeferredInputTable(device);

    m_rtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, false, L"RTV Heap (CPU)");

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
    uint32_t width,
    uint32_t height,
    float time)
{
    auto* cmdList = cl.Get();

    CmdBeginEvent(cmdList, "Renderer");

    if (!m_resourcesReady)
    {
        SetupResources(device, cl, frameIndex);
        m_resourcesReady = true;
    }

    //Bind the heap before drawing
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Per-frame CB (b0)
    const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb = UpdateGlobalConstants(frameIndex, width, height, time);
    
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

    if (m_useDeferred && m_gbufferReady)
    {
        CmdBeginEvent(cmdList, "DeferredPath");

        // Scene table stays the true scene contract in deferred too
        CmdBeginEvent(cmdList, "UpdateSceneTable");
        UpdateSceneTable(device);
        CmdEndEvent(cmdList);

        // GBUFFER PASS 
        // Transitions
        cl.Transition(m_gbuffer0.Tex().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl.Transition(m_gbuffer1.Tex().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl.Transition(m_gbuffer2.Tex().Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (m_depthReady) 
            cl.Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl.FlushBarriers();

        // Clear GBuffer RTs and Depth
        cmdList->ClearRenderTargetView(m_gbuffer0.RTV(), m_gbuffer0.ClearColor(), 0, nullptr);
        cmdList->ClearRenderTargetView(m_gbuffer1.RTV(), m_gbuffer1.ClearColor(), 0, nullptr);
        cmdList->ClearRenderTargetView(m_gbuffer2.RTV(), m_gbuffer2.ClearColor(), 0, nullptr);
        if (m_depthReady) 
            cmdList->ClearDepthStencilView(m_depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Bind MRTs (Multiple Render Targets)
        D3D12_CPU_DESCRIPTOR_HANDLE mrt[3] = 
        { 
            m_gbuffer0.RTV(), 
            m_gbuffer1.RTV(), 
            m_gbuffer2.RTV() 
        };
        cmdList->OMSetRenderTargets(3, mrt, FALSE, m_depthReady ? &m_depthDsv : nullptr);
        
        CmdBeginEvent(cmdList, "GBufferPass");
        m_gbufferPass.Render(cl, width, height, perFrameCb, drawAlloc.gpu, m_scene.table.gpu, m_material, m_quad);
        CmdEndEvent(cmdList);

        // LIGHTING PASS PREP 
        // Remap the Scene Table 
        //CmdBeginEvent(cmdList, "UpdateDeferredInputTable");
        ////UpdateSceneTableForDeferred(device);
		//UpdateDeferredInputTable(device);
        //CmdEndEvent(cmdList);

        // Transition GBuffers to Shader Resource so the lighting pass can read them
        cl.Transition(m_gbuffer0.Tex().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_gbuffer1.Tex().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_gbuffer2.Tex().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (m_depthReady)
            cl.Transition(m_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl.FlushBarriers();

        // LIGHTING PASS
        cmdList->ClearRenderTargetView(backbufferRtv, m_clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr); // No depth for lighting quad
        
        CmdBeginEvent(cmdList, "DeferredLightingPass");
        m_deferredLightPass.Render(cl, width, height, perFrameCb, m_scene.table.gpu, m_deferredInputTable.gpu);
        CmdEndEvent(cmdList);

        CmdEndEvent(cmdList);
    }
    else
    {
        CmdBeginEvent(cmdList, "ForwardPath");
        CmdBeginEvent(cmdList, "UpdateSceneTableForward");
        UpdateSceneTable(device);
        CmdEndEvent(cmdList);

        // Normal Forward Setup
        if (m_depthReady) 
            cl.Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl.FlushBarriers();

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
        CmdEndEvent(cmdList);
        m_forwardPbr.Render(cl, width, height, perFrameCb, drawAlloc.gpu, m_scene.table.gpu, m_material, m_quad);
        CmdEndEvent(cmdList);
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

    CreateOrResizeGBuffers(device, width, height);
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
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

    DirectX::XMStoreFloat4x4(&cb->viewProj, viewProj);
    DirectX::XMStoreFloat4x4(&cb->invViewProj, invViewProj);

    cb->cameraPos = cam.position;
    cb->time = time; 
    cb->frameIndex = frameIndex;

    cb->hasBRDFLut = m_brdfLutTex.IsValid() ? 1 : 0;
    cb->hasIBL = (m_iblDiffuseTex.IsValid() && m_iblSpecularTex.IsValid()) ? 1 : 0;
    cb->_pad0 = 0u;
    
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

      
        if (std::filesystem::exists(content / L"Textures" / L"ibl_brdf_lut.png"))
        {
            m_brdfLutTex.LoadFromFile_DirectXTex(
                device, cl, m_upload, frameIndex,
                content / L"Textures" / L"ibl_brdf_lut.png",
                false,
                L"Tex: BRDF LUT");

        }
        else
        {
            // Log a warning and leave m_brdfLutTex as 'Invalid'
            DebugOutput("Warning: ibl_brdf_lut.png missing. PBR will look incorrect.");
        }

        if (std::filesystem::exists(content / L"Textures" / L"lilienstein_2kblurred.png"))
        {
            m_iblDiffuseTex.LoadFromFile_DirectXTex(
                device, cl, m_upload, frameIndex,
                content / L"Textures" / L"lilienstein_2kblurred.png",
                false,
                L"Tex: IBL Diffuse");
        }
        else
        {
            DebugOutput("Warning: lilienstein_2kblurred.png missing. Using null scene slot.");
        }

        if (std::filesystem::exists(content / L"Textures" / L"lilienstein_2k.png"))
        {
            m_iblSpecularTex.LoadFromFile_DirectXTex(
                device, cl, m_upload, frameIndex,
                content / L"Textures" / L"lilienstein_2k.png",
                false,
                L"Tex: IBL Specular");
        }
        else
        {
            DebugOutput("Warning: lilienstein_2k.png missing. Using null scene slot.");
        }

        // Material handles the table allocation and SRV placement
        m_material.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);
        
        CmdBeginEvent(cl.Get(), "UpdateSceneTable");
        UpdateSceneTable(device);
        CmdEndEvent(cl.Get());

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

    m_gbufferPass.Initialize(
        device,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_D32_FLOAT,
        Paths::ShaderDir());

    m_deferredLightPass.Initialize(
        device,
        m_backbufferFormat,
        Paths::ShaderDir());
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

void Renderer::UpdateSceneTable(ID3D12Device* device)
{
    if (!m_scene.IsValid())
        return;

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SceneCpuHandle = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_scene.table.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    // Start from deterministic null descriptors.
    for (uint32_t i = 0; i < SceneResources::COUNT; ++i)
        CreateNullTexture2DSRV(device, SceneCpuHandle(i), DXGI_FORMAT_R8G8B8A8_UNORM);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.MostDetailedMip = 0;

    // Slot 0: BRDF LUT
    if (m_brdfLutTex.IsValid())
    {
        srv.Format = m_brdfLutTex.SrvFormat();

        device->CreateShaderResourceView(
            m_brdfLutTex.Get(),
            &srv,
            SceneCpuHandle(SceneResources::BRDF_LUT));
    }

    // Slot 1: IBL diffuse latlong
    if (m_iblDiffuseTex.IsValid())
    {
        srv.Format = m_iblDiffuseTex.SrvFormat();
        device->CreateShaderResourceView(
            m_iblDiffuseTex.Get(),
            &srv,
            SceneCpuHandle(SceneResources::IBL_DIFFUSE));
    }

    // Slot 2: IBL specular latlong
    if (m_iblSpecularTex.IsValid())
    {
        srv.Format = m_iblSpecularTex.SrvFormat();
        device->CreateShaderResourceView(
            m_iblSpecularTex.Get(),
            &srv,
            SceneCpuHandle(SceneResources::IBL_SPECULAR));
    }

    // Slot 3: SHADOW_MAP stays null for now TODO implement
}

void Renderer::CreateOrResizeGBuffers(ID3D12Device* device, uint32_t w, uint32_t h)
{
    //clear values
    const float c0[4] = { 0, 0, 0, 0 };
    const float c1[4] = { 0, 0, 0, 0 };
    const float c2[4] = { 0, 0, 0, 0 };

    // NOTE: Deferred pass reuses Scene SRV slots for GBuffer binding:
    // t0 (BRDF_LUT)      > GBuffer0 (BaseColor)
    // t1 (IBL_DIFFUSE)   > GBuffer1 (Normal)
    // t2 (IBL_SPECULAR)  > GBuffer2 (MRAO)
    // This avoids needing a separate root signature   

    m_gbuffer0.Create(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, c0, L"GBuffer0 BaseColor");
    m_gbuffer1.Create(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, c1, L"GBuffer1 Normal");
    m_gbuffer2.Create(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, c2, L"GBuffer2 MRAO");

    m_gbuffer0.CreateRTV(device, m_rtvHeap, L"GBuffer0 RTV");
    m_gbuffer1.CreateRTV(device, m_rtvHeap, L"GBuffer1 RTV");
    m_gbuffer2.CreateRTV(device, m_rtvHeap, L"GBuffer2 RTV");

    if (m_deferredInputTableReady)
    {
        UpdateDeferredInputTable(device);
    }

    m_gbufferReady = true;
}


void Renderer::CreateNullDeferredInputTable(ID3D12Device* device)
{
    if (!m_deferredInputTable.IsValid())
        m_deferredInputTable = m_srvHeap.Allocate(8);

    for (uint32_t i = 0; i < 8; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_deferredInputTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        CreateNullTexture2DSRV(device, h, DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    m_deferredInputTableReady = true;
}

void Renderer::UpdateDeferredInputTable(ID3D12Device* device)
{
    if (!m_deferredInputTable.IsValid())
        return;

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto DeferredCpuHandle = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_deferredInputTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    // Start from deterministic nulls
    for (uint32_t i = 0; i < 8; ++i)
        CreateNullTexture2DSRV(device, DeferredCpuHandle(i), DXGI_FORMAT_R8G8B8A8_UNORM);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.MostDetailedMip = 0;

    // t0 = GBuffer0
    if (m_gbuffer0.Tex().IsValid())
    {
        srv.Format = m_gbuffer0.Tex().SrvFormat();
        device->CreateShaderResourceView(m_gbuffer0.Tex().Get(), &srv, DeferredCpuHandle(0));
    }

    // t1 = GBuffer1
    if (m_gbuffer1.Tex().IsValid())
    {
        srv.Format = m_gbuffer1.Tex().SrvFormat();
        device->CreateShaderResourceView(m_gbuffer1.Tex().Get(), &srv, DeferredCpuHandle(1));
    }

    // t2 = GBuffer2
    if (m_gbuffer2.Tex().IsValid())
    {
        srv.Format = m_gbuffer2.Tex().SrvFormat();
        device->CreateShaderResourceView(m_gbuffer2.Tex().Get(), &srv, DeferredCpuHandle(2));
    }

    // t3 = Depth
    if (m_depth.IsValid())
    {
        srv.Format = m_depth.SrvFormat(); // DXGI_FORMAT_R32_FLOAT
        device->CreateShaderResourceView(m_depth.Get(), &srv, DeferredCpuHandle(3));
    }
}

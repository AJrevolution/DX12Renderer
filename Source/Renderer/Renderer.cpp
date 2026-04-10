#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "DrawConstants.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"
#include <cstring>


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

    CreateOrResizeShadowMap(device);
    

    HRESULT dxrHr = device->QueryInterface(IID_PPV_ARGS(&m_device5));
    if (SUCCEEDED(dxrHr) && m_device5)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))) &&
            opts5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
        {
            DebugOutput("DXR device interface available.");
            m_dxrAvailable = true;
        }
        else
        {
            DebugOutput("DXR not supported on this device. Ray tracing disabled.");
            m_device5.Reset();
        }
    }
    else
    {
        DebugOutput("DXR not supported on this device. Ray tracing disabled.");
    }

    m_frameCount = frameCount;
    m_rtFrames.resize(frameCount);
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
    ID3D12Resource* backbufferResource,
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
    BuildDrawList(time);
    bool renderedWithDxr = false;
    const bool canRunDxr =
        m_useRaytracing &&
        m_dxrAvailable &&
        m_device5 &&
        m_rtPipeline.StateObject();

    if (canRunDxr)
    {
        EnsureRtOutputSize(width, height);
        EnsureRtInstanceData(frameIndex);
        UpdateRtGeometryTable(frameIndex);

        auto cmd4 = GetCommandList4(cl);

        BuildTlasForDrawList(frameIndex, cmd4.Get());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
        cmd4->SetDescriptorHeaps(1, heaps);
        if (m_rtOutputReady && m_rtFrames[frameIndex].tlas.GpuAddress() != 0)
        {
            cmd4->SetPipelineState1(m_rtPipeline.StateObject());
            cmd4->SetComputeRootSignature(m_rtPipeline.GlobalRootSignature());
            cmd4->SetComputeRootDescriptorTable(0, m_rtOutputUav.gpu);
            cmd4->SetComputeRootShaderResourceView(1, m_rtFrames[frameIndex].tlas.GpuAddress());
            cmd4->SetComputeRootConstantBufferView(2, perFrameCb);
            cmd4->SetComputeRootDescriptorTable(3, m_rtFrames[frameIndex].geometryTable.gpu);

            auto tableBase = m_rtPipeline.ShaderTable()->GetGPUVirtualAddress();

            D3D12_DISPATCH_RAYS_DESC rays{};
            rays.RayGenerationShaderRecord.StartAddress = tableBase + m_rtPipeline.RayGenOffset();
            rays.RayGenerationShaderRecord.SizeInBytes = m_rtPipeline.RayGenRecordSize();

            rays.MissShaderTable.StartAddress = tableBase + m_rtPipeline.MissOffset();
            rays.MissShaderTable.SizeInBytes = m_rtPipeline.MissRecordSize();
            rays.MissShaderTable.StrideInBytes = m_rtPipeline.MissRecordSize();

            rays.HitGroupTable.StartAddress = tableBase + m_rtPipeline.HitGroupOffset();
            rays.HitGroupTable.SizeInBytes = m_rtPipeline.HitGroupRecordSize();
            rays.HitGroupTable.StrideInBytes = m_rtPipeline.HitGroupRecordSize();

            rays.Width = width;
            rays.Height = height;
            rays.Depth = 1;

            CmdBeginEvent(cl.Get(), "DXR");
            cmd4->DispatchRays(&rays);
            CmdEndEvent(cl.Get());

            // Copy RT output into backbuffer
            cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl.Transition(backbufferResource, D3D12_RESOURCE_STATE_COPY_DEST);
            cl.FlushBarriers();

            cl.Get()->CopyResource(backbufferResource, m_rtOutput.Get());

            cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.FlushBarriers();
            renderedWithDxr = true;
        }
    }

    if (!renderedWithDxr)
    {
        if (m_enableShadows && m_shadowReady)
        {
            CmdBeginEvent(cmdList, "ShadowPass");

            cl.Transition(m_shadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cl.FlushBarriers();

            cmdList->ClearDepthStencilView(m_shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsv);

            for (const DrawItem& item : m_draws)
            {
                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                m_shadowPass.Render(cl, m_shadowSize, perFrameCb, perDrawCb, *item.mesh);
            }
            CmdEndEvent(cmdList);
        }

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

            for (const DrawItem& item : m_draws)
            {
                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                m_gbufferPass.Render(
                    cl,
                    width,
                    height,
                    perFrameCb,
                    perDrawCb,
                    m_scene.table.gpu,
                    *item.material,
                    *item.mesh);
            }
            CmdEndEvent(cmdList);

            // LIGHTING PASS PREP 
            // Remap the Scene Table 
            CmdBeginEvent(cmdList, "UpdateDeferredInputTable");
            //UpdateSceneTableForDeferred(device);
            UpdateDeferredInputTable(device);
            CmdEndEvent(cmdList);

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

            for (const DrawItem& item : m_draws)
            {
                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                m_forwardPbr.Render(
                    cl,
                    width,
                    height,
                    perFrameCb,
                    perDrawCb,
                    m_scene.table.gpu,
                    *item.material,
                    *item.mesh);
            }
            CmdEndEvent(cmdList);
            CmdEndEvent(cmdList);
        }
    }
    CmdEndEvent(cmdList);
}

void Renderer::OnResize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_widthCached = width;
    m_heightCached = height;

    m_depthReady = false;
    m_gbufferReady = false;
    m_rtOutputReady = false;

    // Recreate depth on resize
    m_depth.CreateDepth(device, width, height, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, L"Depth Buffer");
    CommandList::SetGlobalState(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

    // Allocate the DSV once; reuse handle across recreations
    if (m_depthDsv.ptr == 0)
        m_depthDsv = m_dsvHeap.Allocate(1).cpu;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depth.Get(), &dsv, m_depthDsv);

    m_depthReady = true;

    CreateOrResizeGBuffers(device, width, height);
   
    if (m_dxrAvailable && m_device5)
    {
        //m_rtPipeline.Initialize(m_device5.Get(), Paths::ShaderDir());
        //CreateRtGeometryTable(device);
        CreateRtOutput(device, width, height); 
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateGlobalConstants(uint32_t frameIndex, uint32_t width, uint32_t height, float time)
{
    // The 256 alignment is handled by the allocator, but we ensure the size is also a multiple of 256
    constexpr uint32_t cbSize = (sizeof(PerFrameConstants) + 255) & ~255;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<PerFrameConstants*>(alloc.cpu);
    if (m_autoOrbit)
    {
        m_camYaw = time * 0.35f;

        const float cp = cosf(m_camPitch);
        const float sp = sinf(m_camPitch);
        const float cy = cosf(m_camYaw);
        const float sy = sinf(m_camYaw);

        m_sceneData.camera.position =
        {
            sy * cp * m_camRadius,
            sp * m_camRadius + 1.5f,
            cy * cp * m_camRadius
        };
        m_sceneData.camera.target = { 0.0f, 0.5f, 0.0f };
    }
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

    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&sun.direction));
    XMVECTOR sceneCenter = XMLoadFloat3(&m_sceneBoundsCenter);


    XMVECTOR lightPos = sceneCenter - lightDir * 8.0f;

    XMMATRIX lightView = XMMatrixLookAtLH(
        lightPos,
        sceneCenter,
        XMVectorSet(0, 1, 0, 0));

    XMMATRIX lightProj = XMMatrixOrthographicLH(
        m_sceneBoundsExtent.x * 2.0f,
        m_sceneBoundsExtent.y * 2.0f,
        0.1f,
        20.0f);

    XMMATRIX lightViewProj = lightView * lightProj;

    DirectX::XMStoreFloat4x4(&cb->viewProj, viewProj);
    DirectX::XMStoreFloat4x4(&cb->invViewProj, invViewProj);
    DirectX::XMStoreFloat4x4(&cb->lightViewProj, lightViewProj);

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

    cb->shadowInvSize = 
    {
        1.0f / static_cast<float>(m_shadowSize),
        1.0f / static_cast<float>(m_shadowSize)
    };

    cb->debugView = m_debugView;
    cb->padShadow = 0;

    return alloc.gpu;
}


void Renderer::SetupResources(ID3D12Device* device, CommandList& cl, uint32_t frameIndex)
{
    m_forwardPbr.Initialize(device, m_backbufferFormat, DXGI_FORMAT_D32_FLOAT, Paths::ShaderDir());

    // Create mesh GPU resources (DEFAULT heap VB+IB)
    m_quad.CreateTexturedQuad(device, cl, m_upload, frameIndex);
    m_floor.CreateFloorPlane(device, cl, m_upload, frameIndex);

    if (m_dxrAvailable && m_device5)
    {
        // Transition mesh buffers for DXR geometry reads
        cl.Transition(
            m_quad.VertexBufferResource(),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.Transition(
            m_quad.IndexBufferResource(),
            D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.Transition(
            m_floor.VertexBufferResource(),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.Transition(
            m_floor.IndexBufferResource(),
            D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.FlushBarriers();

        auto cmd4 = GetCommandList4(cl);
 
        {
            AccelerationStructure::GeometryDesc g{};
            g.vertexBuffer = m_quad.VertexBufferResource()->GetGPUVirtualAddress();
            g.vertexCount = m_quad.VertexCount();
            g.vertexStride = m_quad.VertexStride();
            g.indexBuffer = m_quad.IndexBufferResource()->GetGPUVirtualAddress();
            g.indexCount = m_quad.IndexCount();
            g.indexFormat = m_quad.IndexFormat();
            m_blasQuad.BuildBottomLevel(m_device5.Get(), cmd4.Get(), g, L"BLAS Quad");
        }

        {
            AccelerationStructure::GeometryDesc g{};
            g.vertexBuffer = m_floor.VertexBufferResource()->GetGPUVirtualAddress();
            g.vertexCount = m_floor.VertexCount();
            g.vertexStride = m_floor.VertexStride();
            g.indexBuffer = m_floor.IndexBufferResource()->GetGPUVirtualAddress();
            g.indexCount = m_floor.IndexCount();
            g.indexFormat = m_floor.IndexFormat();
            m_blasFloor.BuildBottomLevel(m_device5.Get(), cmd4.Get(), g, L"BLAS Floor");
        }

        m_rtPipeline.Initialize(m_device5.Get(), Paths::ShaderDir());
        //CreateRtGeometryTable(device);
		CreateRtOutput(device, m_widthCached, m_heightCached); //Cached variables changed in OnResize
    }

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
       
        m_floorMaterial.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);


        CmdBeginEvent(cl.Get(), "UpdateSceneTable");
        UpdateSceneTable(device);
        CmdEndEvent(cl.Get());

        // Fill in PBR factors
        m_material.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_material.metallicFactor = 0.5f;
        m_material.roughnessFactor = 0.3f;

        m_floorMaterial.baseColorFactor = { 0.8f, 0.8f, 0.8f, 1.0f };
        m_floorMaterial.metallicFactor = 0.0f;
        m_floorMaterial.roughnessFactor = 0.9f; 
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

    m_shadowPass.Initialize(
        device,
        DXGI_FORMAT_D32_FLOAT,
        Paths::ShaderDir());

    m_matteMaterial.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);
    m_matteMaterial.baseColorFactor = { 0.85f, 0.2f, 0.2f, 1.0f };
    m_matteMaterial.metallicFactor = 0.0f;
    m_matteMaterial.roughnessFactor = 0.95f;

    m_glossyMaterial.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);
    m_glossyMaterial.baseColorFactor = { 0.2f, 0.3f, 0.85f, 1.0f };
    m_glossyMaterial.metallicFactor = 0.0f;
    m_glossyMaterial.roughnessFactor = 0.08f;

    m_metalMaterial.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);
    m_metalMaterial.baseColorFactor = { 0.9f, 0.9f, 0.9f, 1.0f };
    m_metalMaterial.metallicFactor = 1.0f;
    m_metalMaterial.roughnessFactor = 0.35f;

    m_normalStrongMaterial.UpdateDescriptorTable(device, m_srvHeap, &m_albedoTex, &m_normalTex, &m_metalRoughTex);
    m_normalStrongMaterial.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_normalStrongMaterial.metallicFactor = 0.0f;
    m_normalStrongMaterial.roughnessFactor = 0.45f;

    const auto materialsDir = content / L"Materials";

    LoadMaterialFromFolder(
        device, cl, frameIndex,
        materialsDir / L"Mat_Matte",
        m_matteMaterial,
        m_matteBaseTex, m_matteNormalTex, m_matteOrmTex,
		L"rock_wall_16_diff_1024.png", L"rock_wall_16_nor_dx_1024.png", L"rock_wall_16_arm_1024.png",
        { 1,1,1,1 }, 0.0f, 0.95f, 
        true);

    LoadMaterialFromFolder(
        device, cl, frameIndex,
        materialsDir / L"Mat_Glossy",
        m_glossyMaterial,
        m_glossyBaseTex, m_glossyNormalTex, m_glossyOrmTex,
        L"wood_table_001_diff_1024.png", L"wood_table_001_nor_1024.png", L"wood_table_001_arm_dx_1024.png",
        { 1,1,1,1 }, 0.0f, 0.08f, 
        true);

    LoadMaterialFromFolder(
        device, cl, frameIndex,
        materialsDir / L"Mat_Metal",
        m_metalMaterial,
        m_metalBaseTex, m_metalNormalTex, m_metalOrmTex,
        L"metal_plate_02_diff_1024.png", L"metal_plate_02_nor_dx_1024.png", L"metal_plate_02_arm_1024.png",
        { 1,1,1,1 }, 1.0f, 0.35f, 
        true);
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

    // Slot 3: SHADOW_MAP 
    if (m_enableShadows && m_shadowMap.IsValid())
    {
        srv.Format = m_shadowMap.SrvFormat(); 
        device->CreateShaderResourceView(
            m_shadowMap.Get(),
            &srv,
            SceneCpuHandle(SceneResources::SHADOW_MAP));
    }
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

// Shadow bring-up settings:
// - m_shadowSize controls PCF texel scale via PerFrameConstants.shadowInvSize
// - ShadowPass PSO uses rasterizer depth bias + slope-scaled depth bias
// - Forward/Deferred sample shadow map with point taps for deterministic manual PCF
// minimal, will be refined later.
void Renderer::CreateOrResizeShadowMap(ID3D12Device* device)
{
    m_shadowMap.CreateDepth(
        device,
        m_shadowSize,
        m_shadowSize,
        DXGI_FORMAT_R32_TYPELESS,
        DXGI_FORMAT_D32_FLOAT,
        L"Shadow Map");

    if (m_shadowDsv.ptr == 0)
    {
        auto alloc = m_dsvHeap.Allocate(1);
        m_shadowDsv = alloc.cpu;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;

    device->CreateDepthStencilView(m_shadowMap.Get(), &dsv, m_shadowDsv);
    m_shadowReady = true;
}

void Renderer::BuildDrawList(float time)
{
    m_draws.clear();
    m_draws.reserve(4);

    using namespace DirectX;

    const float oscillation = sinf(time * 1.5f);
    const float rotationAngle = oscillation * 1.1f;

    {
        DrawItem item{};
        item.mesh = &m_floor;
        item.material = &m_floorMaterial;
        XMStoreFloat4x4(&item.world, XMMatrixIdentity());
        m_draws.push_back(item);
    }

    // Center rotating metal test object
    {
        DrawItem item{};
        item.mesh = &m_quad;
        item.material = &m_metalMaterial;
        XMMATRIX world =
            XMMatrixRotationY(rotationAngle) *
            XMMatrixTranslation(0.0f, 0.5f, 0.0f);
        XMStoreFloat4x4(&item.world, world);
        m_draws.push_back(item);
    }

    // Left matte reference
    {
        DrawItem item{};
        item.mesh = &m_quad;
        item.material = &m_matteMaterial;
        XMMATRIX world =
            XMMatrixScaling(0.9f, 0.9f, 0.9f) *
            XMMatrixTranslation(-1.75f, 0.5f, 0.0f);
        XMStoreFloat4x4(&item.world, world);
        m_draws.push_back(item);
    }

    // Right glossy reference
    {
        DrawItem item{};
        item.mesh = &m_quad;
        item.material = &m_glossyMaterial;
        XMMATRIX world =
            XMMatrixScaling(0.9f, 0.9f, 0.9f) *
            XMMatrixTranslation(1.75f, 0.5f, 0.0f);
        XMStoreFloat4x4(&item.world, world);
        m_draws.push_back(item);
    }

}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadPerDrawConstants(
    uint32_t frameIndex,
    const DrawItem& item)
{
    auto alloc = m_upload.Allocate(frameIndex, sizeof(PerDrawConstants), 256);
    auto* dc = reinterpret_cast<PerDrawConstants*>(alloc.cpu);

    dc->world = item.world;
    dc->materialIndex = 0;
    dc->baseColorFactor = item.material->baseColorFactor;
    dc->metallicFactor = item.material->metallicFactor;
    dc->roughnessFactor = item.material->roughnessFactor;

    return alloc.gpu;
}

bool Renderer::LoadMaterialFromFolder(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    const std::filesystem::path& folder,
    Material& outMaterial,
    Texture& outBaseColor,
    Texture& outNormal,
    Texture& outOrm,
    const std::wstring& baseName,
    const std::wstring& normalName,
    const std::wstring& ormName,
    const DirectX::XMFLOAT4& baseColorFactor,
    float metallicFactor,
    float roughnessFactor,
    bool requireAll)
{
    if (!std::filesystem::exists(folder))
        return false;

    const auto basePath = folder / baseName;
    const auto normalPath = folder / normalName;
    const auto ormPath = folder / ormName;

    const bool hasBase = std::filesystem::exists(basePath);
    const bool hasNormal = std::filesystem::exists(normalPath);
    const bool hasOrm = std::filesystem::exists(ormPath);

    const bool allPresent = hasBase && hasNormal && hasOrm;
    const bool anyPresent = hasBase || hasNormal || hasOrm;

    if (requireAll && !allPresent)
    {
        DebugOutput(std::format(
            "Material folder incomplete: {} (base={}, normal={}, orm={})",
            folder.string(),
            hasBase ? "yes" : "no",
            hasNormal ? "yes" : "no",
            hasOrm ? "yes" : "no"));
        return false;
    }

    if (!requireAll && !anyPresent)
        return false;

    if (hasBase)
        outBaseColor.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex, basePath, true, L"Mat BaseColor");

    if (hasNormal)
        outNormal.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex, normalPath, false, L"Mat Normal");

    if (hasOrm)
        outOrm.LoadFromFile_DirectXTex(device, cl, m_upload, frameIndex, ormPath, false, L"Mat ORM");

    outMaterial.UpdateDescriptorTable(device, m_srvHeap, &outBaseColor, &outNormal, &outOrm);
    outMaterial.baseColorFactor = baseColorFactor;
    outMaterial.metallicFactor = metallicFactor;
    outMaterial.roughnessFactor = roughnessFactor;

    return true;
}

bool Renderer::LoadMaterialFromFolder(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    const std::filesystem::path& folder,
    Material& outMaterial,
    Texture& outBaseColor,
    Texture& outNormal,
    Texture& outOrm,
    const DirectX::XMFLOAT4& baseColorFactor,
    float metallicFactor,
    float roughnessFactor,
    bool requireAll)
{
    return LoadMaterialFromFolder(
        device,
        cl,
        frameIndex,
        folder,
        outMaterial,
        outBaseColor,
        outNormal,
        outOrm,
        L"basecolor.png",
        L"normal.png",
        L"orm.png",
        baseColorFactor,
        metallicFactor,
        roughnessFactor,
        requireAll);
}

void Renderer::BuildTlasForDrawList(uint32_t frameIndex, ID3D12GraphicsCommandList4* cmd4)
{
    std::vector<AccelerationStructure::InstanceDesc> instances;
    instances.reserve(m_draws.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_draws.size()); ++i)
    {
        const DrawItem& item = m_draws[i];
        AccelerationStructure::InstanceDesc inst{};
        inst.instanceID = i;
        inst.hitGroupIndex = 0;
        inst.mask = 0xFF;

        inst.blasAddress = (item.mesh == &m_floor)
            ? m_blasFloor.GpuAddress()
            : m_blasQuad.GpuAddress();

        const auto& m = item.world;
        inst.transform[0] = m._11; inst.transform[1] = m._12; inst.transform[2] = m._13; inst.transform[3] = m._41;
        inst.transform[4] = m._21; inst.transform[5] = m._22; inst.transform[6] = m._23; inst.transform[7] = m._42;
        inst.transform[8] = m._31; inst.transform[9] = m._32; inst.transform[10] = m._33; inst.transform[11] = m._43;

        instances.push_back(inst);
    }

    if (!instances.empty())
    {
        m_rtFrames[frameIndex].tlas.BuildTopLevel(
            m_device5.Get(),
            cmd4,
            instances.data(),
            static_cast<uint32_t>(instances.size()),
            L"TLAS Scene");
    }
}

ComPtr<ID3D12GraphicsCommandList4> Renderer::GetCommandList4(CommandList& cl)
{
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    ThrowIfFailed(cl.Get()->QueryInterface(IID_PPV_ARGS(&cmd4)), "QI(ID3D12GraphicsCommandList4)");
    return cmd4;
}


void Renderer::EnsureRtOutputSize(uint32_t width, uint32_t height)
{
    if (!m_device5)
        return;

    const bool sizeMismatch =
        (m_rtOutputWidth != width) ||
        (m_rtOutputHeight != height);

    if (!m_rtOutput || !m_rtOutputReady || sizeMismatch)
    {
        CreateRtOutput(m_device5.Get(), width, height);
    }
}

void Renderer::CreateRtOutput(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtOutputReady = false;
    m_rtOutput.Reset();

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        width,
        height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_rtOutput)),
        "Create RT output");

    SetD3D12ObjectName(m_rtOutput.Get(), L"RT Output");
    CommandList::SetGlobalState(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!m_rtOutputUav.IsValid())
        m_rtOutputUav = m_srvHeap.Allocate(1);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateUnorderedAccessView(m_rtOutput.Get(), nullptr, &uav, m_rtOutputUav.cpu);
    m_rtOutputWidth = width;
    m_rtOutputHeight = height;

    m_rtOutputReady = true;
}

void Renderer::EnsureRtInstanceData(uint32_t frameIndex)
{
    auto& frame = m_rtFrames[frameIndex];
    const uint32_t requiredCount = std::max<uint32_t>(1u, static_cast<uint32_t>(m_draws.size()));

    if (!frame.instanceDataUpload || frame.capacity < requiredCount)
    {
        frame.capacity = std::max<uint32_t>(requiredCount, frame.capacity ? frame.capacity * 2u : 8u);

        const uint64_t bufferSize = uint64_t(frame.capacity) * uint64_t(sizeof(RTInstanceData));

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        ThrowIfFailed(
            m_device5->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&frame.instanceDataUpload)),
            "Create RT instance data upload");

        SetD3D12ObjectName(frame.instanceDataUpload.Get(), L"RT Instance Data Upload");

        if (!frame.instanceDataSrv.IsValid())
            frame.instanceDataSrv = m_srvHeap.Allocate(1);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = frame.capacity;
        srv.Buffer.StructureByteStride = sizeof(RTInstanceData);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        m_device5->CreateShaderResourceView(frame.instanceDataUpload.Get(), &srv, frame.instanceDataSrv.cpu);
    }

    D3D12_RANGE readRange{ 0, 0 };
    RTInstanceData* dst = nullptr;
    ThrowIfFailed(
        frame.instanceDataUpload->Map(0, &readRange, reinterpret_cast<void**>(&dst)),
        "Map RT instance data upload");

    std::memset(dst, 0, sizeof(RTInstanceData) * frame.capacity);

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_draws.size()); ++i)
    {
        const DrawItem& item = m_draws[i];
        const Material* mat = item.material;

        RTInstanceData data{};

        data.baseColorFactor = mat ? mat->baseColorFactor : DirectX::XMFLOAT4(1, 1, 1, 1);
        data.metallic = mat ? mat->metallicFactor : 0.0f;
        data.roughness = mat ? mat->roughnessFactor : 0.5f;

        data.meshType = (item.mesh == &m_floor) ? 0u : 1u;
        data.materialId = i;

        dst[i] = data;
    }

    D3D12_RANGE writtenRange{ 0, SIZE_T(sizeof(RTInstanceData) * m_draws.size()) };
    frame.instanceDataUpload->Unmap(0, &writtenRange);
}

void Renderer::UpdateRtGeometryTable(uint32_t frameIndex)
{
    auto& frame = m_rtFrames[frameIndex];

    if (!frame.geometryTable.IsValid())
        frame.geometryTable = m_srvHeap.Allocate(5);

    auto HandleAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.geometryTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    // t1 = quad vertices
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.Buffer.FirstElement = 0;
        s.Buffer.NumElements = m_quad.VertexCount();
        s.Buffer.StructureByteStride = m_quad.VertexStride();
        s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        m_device5->CreateShaderResourceView(m_quad.VertexBufferResource(), &s, HandleAt(0));
    }

    // t2 = quad indices (raw)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.Buffer.FirstElement = 0;
        s.Buffer.NumElements = (m_quad.IndexCount() * 2 + 3) / 4;
        s.Buffer.StructureByteStride = 0;
        s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device5->CreateShaderResourceView(m_quad.IndexBufferResource(), &s, HandleAt(1));
    }

    // t3 = floor vertices
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.Buffer.FirstElement = 0;
        s.Buffer.NumElements = m_floor.VertexCount();
        s.Buffer.StructureByteStride = m_floor.VertexStride();
        s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        m_device5->CreateShaderResourceView(m_floor.VertexBufferResource(), &s, HandleAt(2));
    }

    // t4 = floor indices (raw)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.Buffer.FirstElement = 0;
        s.Buffer.NumElements = (m_floor.IndexCount() * 2 + 3) / 4;
        s.Buffer.StructureByteStride = 0;
        s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device5->CreateShaderResourceView(m_floor.IndexBufferResource(), &s, HandleAt(3));
    }

    // t5 = per-frame instance data
    //m_device5->CopyDescriptorsSimple( //crashes
    //    1,
    //    HandleAt(4),
    //    frame.instanceDataSrv.cpu,
    //    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // INSTEAD of CopyDescriptorsSimple, create SRV directly into the table
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = frame.capacity;
        srv.Buffer.StructureByteStride = sizeof(RTInstanceData);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        m_device5->CreateShaderResourceView(
            frame.instanceDataUpload.Get(),
            &srv,
            HandleAt(4) 
        );
    }
}
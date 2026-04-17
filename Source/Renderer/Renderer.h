#pragma once
#include <filesystem>
#include <vector>
#include "Common.h"
#include "Source/Renderer/Passes/TrianglePass.h"
#include "Source/Renderer/Passes/ForwardPBRPass.h"
#include "GPUMarkers.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include "Source/RHI/Resources/Texture.h"
#include "Source/Scene/Mesh.h"
#include "Source/Scene/Material.h"
#include "Source/Renderer/SceneResources.h" 
#include "Source/Renderer/SceneData.h"
#include "Source/RHI/Resources/RenderTarget.h"
#include "Source/Renderer/Passes/GBufferPass.h"
#include "Source/Renderer/Passes/DeferredLightingPass.h"
#include "Source/Renderer/Passes/ShadowPass.h"
#include "Source/RHI/Raytracing/AccelerationStructure.h"
#include "Source/RHI/Raytracing/RaytracingPipeline.h"
#include "Source/RHI/Pipeline/RTInstanceData.h"

class Renderer
{
public:
    void Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount);

    void BeginFrame(uint32_t frameIndex);

    void OnResize(ID3D12Device* device, uint32_t width, uint32_t height);

    void RenderFrame(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        ID3D12Resource* backbufferResource,
        uint32_t width,
        uint32_t height,
        float time
    );
    
    void SetupResources(ID3D12Device* device, CommandList& cl, uint32_t frameIndex);



private:
    struct DrawItem
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        DirectX::XMFLOAT4X4 world{};
    };

    std::vector<DrawItem> m_draws;

    void BuildDrawList(float time);
    D3D12_GPU_VIRTUAL_ADDRESS UploadPerDrawConstants(uint32_t frameIndex, const DrawItem& item);

    bool LoadMaterialFromFolder(
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
        bool requireAll = true);

    bool LoadMaterialFromFolder(
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
        bool requireAll = true);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateGlobalConstants(uint32_t frameIndex, uint32_t width, uint32_t height, float time);
    void CreateNullSceneTable(ID3D12Device* device);
    void UpdateSceneTable(ID3D12Device* device);
    void CreateOrResizeGBuffers(ID3D12Device* device, uint32_t w, uint32_t h);
    void CreateOrResizeShadowMap(ID3D12Device* device);
    void CreateNullDeferredInputTable(ID3D12Device* device);
    void UpdateDeferredInputTable(ID3D12Device* device);

    void CreateRtOutput(ID3D12Device* device, uint32_t width, uint32_t height);
    void BuildTlasForDrawList(uint32_t frameIndex, ID3D12GraphicsCommandList4* cmd4);
    ComPtr<ID3D12GraphicsCommandList4> GetCommandList4(CommandList& cl);

    void EnsureRtOutputSize(uint32_t width, uint32_t height);
    void EnsureRtInstanceData(uint32_t frameIndex);
    void UpdateRtGeometryTable(uint32_t frameIndex);

    void WriteRtTextureSrv(
        ID3D12Device* device,
        D3D12_CPU_DESCRIPTOR_HANDLE dst,
        const Texture& texture) const;

    void CreateRtFallbackTextures(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex);

    uint32_t GetRtMaterialId(const Material* material) const;

    TrianglePass m_triangle;
    UploadArena  m_upload;
    DXGI_FORMAT  m_backbufferFormat = DXGI_FORMAT_UNKNOWN;

    ForwardPBRPass m_forwardPbr;
    SceneResources m_scene;
    SceneData m_sceneData;
    float m_camYaw = 0.0f;
    float m_camPitch = -0.25f;
    float m_camRadius = 4.0f;
    bool  m_autoOrbit = true;

    DescriptorAllocator::Allocation m_deferredInputTable; // space1 for deferred lighting only
    bool m_deferredInputTableReady = false;

    GBufferPass m_gbufferPass;
    DeferredLightingPass m_deferredLightPass;
    bool m_useDeferred = false;

	ShadowPass m_shadowPass;

    DescriptorAllocator m_dsvHeap;
    Texture m_depth;
    D3D12_CPU_DESCRIPTOR_HANDLE m_depthDsv{};
    bool m_depthReady = false;

    Texture m_shadowMap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_shadowDsv{};
    uint32_t m_shadowSize = 2048;
    bool m_shadowReady = false;

    DescriptorAllocator m_srvHeap;
    Texture m_testTexture;
    DescriptorAllocator::Allocation m_testTextureSrv;
    
    DescriptorAllocator m_rtvHeap;

    RenderTarget m_gbuffer0;
    RenderTarget m_gbuffer1;
    RenderTarget m_gbuffer2;
    bool m_gbufferReady = false;

    bool m_resourcesReady = false;


    float m_clearColor[4] = { 0.08f, 0.10f, 0.14f, 1.0f };

    Mesh        m_quad;
    Material    m_material;
    Texture     m_albedoTex;
    Texture     m_normalTex;
    Texture     m_metalRoughTex;
    Texture     m_brdfLutTex;
    Texture     m_iblDiffuseTex;
    Texture     m_iblSpecularTex;
    bool        m_sceneReady = false;

    Material m_matteMaterial;
    Material m_glossyMaterial;
    Material m_metalMaterial;
    Material m_normalStrongMaterial;
    Texture m_matteBaseTex, m_matteNormalTex, m_matteOrmTex;
    Texture m_glossyBaseTex, m_glossyNormalTex, m_glossyOrmTex;
    Texture m_metalBaseTex, m_metalNormalTex, m_metalOrmTex;

    Texture m_rtFallbackWhiteTex;
    Texture m_rtFallbackFlatNormalTex;
    Texture m_rtFallbackOrmTex;

    Mesh m_floor;
    Material m_floorMaterial;
    
    DirectX::XMFLOAT3 m_sceneBoundsCenter = { 0.0f, 0.5f, 0.0f };
    DirectX::XMFLOAT3 m_sceneBoundsExtent = { 3.5f, 3.5f, 3.5f };

    bool m_enableShadows = true;
    uint32_t m_debugView = 0;

	bool m_useRaytracing = false; // Toggle for raytracing vs rasterization (for testing/debugging)
    bool m_dxrAvailable = false;

    ComPtr<ID3D12Device5> m_device5;

    AccelerationStructure m_blasQuad;
    AccelerationStructure m_blasFloor;
    struct FrameRaytracingResources
    {
        AccelerationStructure tlas;
        ComPtr<ID3D12Resource> instanceDataUpload; // upload heap
        DescriptorAllocator::Allocation instanceDataSrv{}; 

        // per-frame RT table: geometry + instance data + material textures
        DescriptorAllocator::Allocation geometryTable{};

        uint32_t capacity = 0;
    };

    
    std::vector<FrameRaytracingResources> m_rtFrames;
    uint32_t m_frameCount = 0;

    RaytracingPipeline m_rtPipeline;

    // RT output
    ComPtr<ID3D12Resource> m_rtOutput;
    DescriptorAllocator::Allocation m_rtOutputUav{};
    uint32_t m_rtOutputWidth = 0;
    uint32_t m_rtOutputHeight = 0;
    uint32_t m_rtMaterialCount = 4;
    bool m_rtOutputReady = false;

    uint32_t m_widthCached = 1;
    uint32_t m_heightCached = 1;

    static constexpr uint32_t kMaxRtMaterials = 8;
    static constexpr uint32_t kRtGeometrySrvCount = 5; // t1..t5
    static constexpr uint32_t kRtTexturesPerMaterial = 3;

    static constexpr uint32_t kRtMaterialFloor = 0;
    static constexpr uint32_t kRtMaterialMetal = 1;
    static constexpr uint32_t kRtMaterialMatte = 2;
    static constexpr uint32_t kRtMaterialGlossy = 3;

};

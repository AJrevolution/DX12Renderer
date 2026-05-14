#pragma once
#include <filesystem>
#include <vector>
#include <array>
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
#include "Source/Renderer/Passes/RtDenoisePass.h"
#include "Source/Renderer/Passes/RtTemporalPass.h"
#include "Source/Renderer/Passes/RtAtrousPass.h"
#include "Source/Renderer/Passes/RtHistorySelectPass.h"

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
    struct RtTemporalConstants
    {
        DirectX::XMFLOAT4X4 currInvViewProj{};
        DirectX::XMFLOAT4X4 prevViewProj{};

        DirectX::XMFLOAT2 invResolution{};
        float temporalAlpha = 0.10f;
        float depthSigma = 0.02f;
        float normalSigma = 0.25f;
        float roughnessSigma = 0.15f;
        float specDirSigma = 0.08f;
        float specDirRoughCutoff = 0.35f;

        uint32_t temporalEnabled = 1;
        uint32_t historyValid = 0;
        uint32_t debugView = 0;
        uint32_t pad0 = 0;

        uint32_t reprojectRadius = 1;
        float reprojectMinConf = 0.25f;
        uint32_t pad1[2] = {};

        DirectX::XMFLOAT4 currCameraPos{};
        DirectX::XMFLOAT4 prevCameraPos{};
    };
    static_assert((sizeof(RtTemporalConstants) % 16) == 0, "RtTemporalConstants must be 16-byte aligned.");

    struct RtAtrousConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t iterationIndex = 0;
        uint32_t stepWidth = 1;

        float sigmaDepth = 0.02f;
        float sigmaNormal = 0.25f;
        float varianceScale = 1.0f;
        uint32_t useMoments = 1;

        uint32_t finalOutputSrgb = 1;
        uint32_t debugView = 0;
        uint32_t pad0[2] = {};
    };

    struct DrawItem
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        DirectX::XMFLOAT4X4 world{};
    };

    struct RtHistorySelectConstants
    {
        float roughnessThreshold = 0.25f;
        float roughnessRange = 0.20f;
        uint32_t debugView = 0;
        uint32_t pad0 = 0;
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

    void CreateRtAccum(ID3D12Device* device, uint32_t width, uint32_t height);
    void ResetRtAccumulation();
    D3D12_CPU_DESCRIPTOR_HANDLE RtUavCpuAt(uint32_t slot) const;

    void CreateRtAovs(ID3D12Device* device, uint32_t width, uint32_t height);
    bool UpdateRtDenoiseSrvTable(uint32_t frameIndex, ID3D12Device* device, ID3D12Resource* signalResource);
    
    void CreateRtHistoryResources(ID3D12Device* device, uint32_t width, uint32_t height);

    bool UpdateRtSvgfSrvTable(
        uint32_t frameIndex,
        uint32_t iterationIndex,
        ID3D12Device* device,
        ID3D12Resource* signalResource,
        ID3D12Resource* momentsResource);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtAtrousConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        uint32_t iterationIndex,
        bool useMoments,
        bool finalOutputSrgb);

    void UpdateRtSvgfPingUavTable(ID3D12Device* device);

    bool UpdateRtTemporalTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* prevAccumResource,
        ID3D12Resource* prevMomentsResource,
        ID3D12Resource* outAccumResource,
        ID3D12Resource* outMomentsResource,
        DescriptorAllocator::Allocation& srvTable,
        DescriptorAllocator::Allocation& uavTable);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtTemporalConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        float temporalAlpha,
        float roughnessSigma);

    D3D12_GPU_DESCRIPTOR_HANDLE RtSvgfPingUavGpuAt(uint32_t i) const;


    bool UpdateRtHistorySelectTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* stableSignal,
        ID3D12Resource* responsiveSignal);

    TrianglePass m_triangle;
    UploadArena  m_upload;
    DXGI_FORMAT  m_backbufferFormat = DXGI_FORMAT_UNKNOWN;

    ForwardPBRPass m_forwardPbr;
    SceneResources m_scene;
    SceneData m_sceneData;
    float m_camYaw = 0.0f;
    float m_camPitch = -0.25f;
    float m_camRadius = 4.0f;
    
    //Toggles
    uint32_t m_debugView = 0;
    bool  m_autoOrbit = true;
    bool m_pauseAnimation = false;
    bool m_useRaytracing = false;        // Toggle for raytracing vs rasterization (for testing/debugging)
    bool m_rtAccumulate = true;          // validation / progressive mode
    
    uint32_t m_rtSamplesPerFrame = 1;    // 1..N
    uint32_t m_rtMaxSamples = 256;       // hard cap

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

    bool     m_rtEnableIndirect = true;  // 1-bounce diffuse GI
    float    m_rtIndirectScale = 1.0f;   // tuning knob

    bool  m_prevRtEnableIndirect = true;
    float m_prevRtIndirectScale = 1.0f;

    Mesh m_floor;
    Material m_floorMaterial;
    
    DirectX::XMFLOAT3 m_sceneBoundsCenter = { 0.0f, 0.5f, 0.0f };
    DirectX::XMFLOAT3 m_sceneBoundsExtent = { 3.5f, 3.5f, 3.5f };

    bool m_enableShadows = true;

    bool m_dxrAvailable = false;

    float m_frozenTime = 0.0f;
    bool  m_wasPaused = false;

    ComPtr<ID3D12Device5> m_device5;

    AccelerationStructure m_blasQuad;
    AccelerationStructure m_blasFloor;
    
    static constexpr uint32_t kMaxRtAtrousIterations = 4;

    struct FrameRaytracingResources
    {
        AccelerationStructure tlas;
        ComPtr<ID3D12Resource> instanceDataUpload; // upload heap
        DescriptorAllocator::Allocation instanceDataSrv{}; 

        // per-frame RT table: geometry + instance data + material textures
        DescriptorAllocator::Allocation geometryTable{};

        DescriptorAllocator::Allocation temporalStableSrvTable{}; // 7 SRVs
        DescriptorAllocator::Allocation temporalStableUavTable{}; // 3 UAVs

        DescriptorAllocator::Allocation temporalRespSrvTable{};   // 7 SRVs
        DescriptorAllocator::Allocation temporalRespUavTable{};   // 3 UAVs

        DescriptorAllocator::Allocation historySelectSrvTable{};  // 4 SRVs
        DescriptorAllocator::Allocation historySelectUavTable{};  // 2 UAVs
        DescriptorAllocator::Allocation denoiseSrvTable{};  // 3 SRVs

        // Per-frame, per-iteration SVGF input tables.
        std::array<DescriptorAllocator::Allocation, kMaxRtAtrousIterations> svgfSrvTables{};

        uint32_t capacity = 0;
    };

    
    std::vector<FrameRaytracingResources> m_rtFrames;
    uint32_t m_frameCount = 0;

    RaytracingPipeline m_rtPipeline;

    // RT output
    ComPtr<ID3D12Resource> m_rtOutput;
    // Contiguous UAV table:
    // u0 = m_rtOutput
    // u1 = m_rtAccum
    DescriptorAllocator::Allocation m_rtOutputUav{};
    uint32_t m_rtOutputWidth = 0;
    uint32_t m_rtOutputHeight = 0;
    // Progressive accumulation target
    ComPtr<ID3D12Resource> m_rtAccum;
    bool m_rtAccumReady = false;
    uint32_t m_rtMaterialCount = 4;
    bool m_rtOutputReady = false;
    uint32_t m_rtSampleIndex = 0;
    uint32_t m_rtDispatchSampleIndex = 0;
    uint32_t m_rtResetId = 0;
    bool m_rtAccumulateThisFrame = false;
    bool m_rtAccumulatingLastFrame = false;

    bool m_rtHistoryValid = false;
    uint32_t m_prevRtDebugView = 0;
    float m_prevRtCamYaw = 0.0f;
    float m_prevRtCamPitch = 0.0f;
    float m_prevRtCamRadius = 0.0f;
    std::vector<DirectX::XMFLOAT4X4> m_prevRtWorlds;
    std::vector<const Material*> m_prevRtMaterials;

    uint32_t m_widthCached = 1;
    uint32_t m_heightCached = 1;

    static constexpr uint32_t kMaxRtMaterials = 8;
    static constexpr uint32_t kRtGeometrySrvCount = 5; // t1..t5
    static constexpr uint32_t kRtTexturesPerMaterial = 3;

    static constexpr uint32_t kRtMaterialFloor = 0;
    static constexpr uint32_t kRtMaterialMetal = 1;
    static constexpr uint32_t kRtMaterialMatte = 2;
    static constexpr uint32_t kRtMaterialGlossy = 3;

    static constexpr uint32_t kRtIblSrvCount = 3;
    static constexpr uint32_t kRtSrvTableCount =
        kRtGeometrySrvCount + (kRtTexturesPerMaterial * kMaxRtMaterials) + kRtIblSrvCount;

    static constexpr uint32_t kRtSrv_BrdfLut =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 0; // t30
    static constexpr uint32_t kRtSrv_IblDiff =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 1; // t31
    static constexpr uint32_t kRtSrv_IblSpec =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 2; // t32



    ComPtr<ID3D12Resource> m_rtAovNormal;
    ComPtr<ID3D12Resource> m_rtAovDepth;
    bool m_rtAovReady = false;

    //DescriptorAllocator::Allocation m_rtDenoiseSrvTable{};
    RtDenoisePass m_rtDenoisePass;

    bool  m_rtDenoise = true;
    int   m_rtDenoiseRadius = 2;
    float m_rtDenoiseSigmaDepth = 0.02f;
    float m_rtDenoiseSigmaNormal = 0.25f;

    //bool m_rtDenoiseSrvTableReady = false;

    bool m_prevRtHasIbl = false;
    bool m_prevRtHasBrdfLut = false;

    RtTemporalPass m_rtTemporalPass;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryAccum{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryNormal{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryDepth{};
    bool m_rtTemporalHistoryValid = false;
    uint32_t m_rtHistoryReadIndex = 0;

    bool  m_rtTemporal = true;
    float m_rtTemporalAlpha = 0.10f;
    float m_rtTemporalDepthSigma = 0.02f;
    float m_rtTemporalNormalSigma = 0.25f;

    DirectX::XMFLOAT4X4 m_currViewProj{};
    DirectX::XMFLOAT4X4 m_currInvViewProj{};
    DirectX::XMFLOAT4X4 m_prevViewProj{};

    RtAtrousPass m_rtAtrousPass;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryMoments{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtSvgfPing{};

    bool m_rtSvgf = true;
    uint32_t m_rtAtrousIterations = 2;
    float m_rtVarianceScale = 1.0f;

    bool m_prevRtSvgf = true;
    uint32_t m_prevRtAtrousIterations = 2;
    float m_prevRtVarianceScale = 1.0f;

    DescriptorAllocator::Allocation m_rtSvgfPingUavTable{};

    float m_rtTemporalRoughnessSigma = 0.15f;
    float m_prevRtTemporalRoughnessSigma = 0.15f;

    RtHistorySelectPass m_rtHistorySelectPass;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryAccumResp{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryMomentsResp{};

    float m_rtTemporalAlphaResp = 0.35f;
    float m_rtTemporalRoughnessSigmaResp = 0.08f;

    float m_rtHistorySelectThreshold = 0.25f;
    float m_rtHistorySelectRange = 0.20f;

    float m_prevRtTemporalAlpha = 0.10f;
    float m_prevRtTemporalAlphaResp = 0.35f;
    float m_prevRtTemporalRoughnessSigmaResp = 0.08f;
    float m_prevRtHistorySelectThreshold = 0.25f;
    float m_prevRtHistorySelectRange = 0.20f;

    float m_rtTemporalSpecDirSigma = 0.08f;
    float m_rtTemporalSpecDirRoughCutoff = 0.35f;

    float m_prevRtTemporalSpecDirSigma = 0.08f;
    float m_prevRtTemporalSpecDirRoughCutoff = 0.35f;

    DirectX::XMFLOAT3 m_currRtCameraPos{};
    DirectX::XMFLOAT3 m_prevRtCameraPos{};

    uint32_t m_rtTemporalReprojectRadius = 1;
    float    m_rtTemporalReprojectMinConf = 0.25f;

    uint32_t m_prevRtTemporalReprojectRadius = 1;
    float    m_prevRtTemporalReprojectMinConf = 0.25f;

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtHistorySelectConstants(uint32_t frameIndex);
};

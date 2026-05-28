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
#include "Source/Renderer/Passes/RtCombinePass.h"
#include "Source/Renderer/Passes/RtMotionDilatePass.h"

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
        float motionConfMin = 0.20f;
        float motionConfPower = 1.0f;

        float varianceScale = 16.0f;
        float varianceBias = 0.0f;
        float varianceAlphaBoost = 0.5f;
        uint32_t enableVarianceBoost = 1;

        DirectX::XMFLOAT4 currCameraPos{};
        DirectX::XMFLOAT4 prevCameraPos{};
    };
    static_assert((sizeof(RtTemporalConstants) % 16) == 0, "RtTemporalConstants must be 16-byte aligned.");

    struct RtRayGenConstants
    {
        DirectX::XMFLOAT4X4 prevViewProj{};
        DirectX::XMFLOAT4X4 currViewProj{};
        DirectX::XMFLOAT4 prevCameraPos{};
        DirectX::XMFLOAT4 currCameraPos{};

        uint32_t hasPrevMotion = 0;
        uint32_t pad0[3] = {};
    };
    static_assert((sizeof(RtRayGenConstants) % 16) == 0, "RtRayGenConstants must be 16-byte aligned.");
    
    struct RtAtrousConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t iterationIndex = 0;
        uint32_t stepWidth = 1;

        float sigmaDepth = 0.02f;
        float sigmaNormal = 0.25f;
        float varianceScale = 1.0f;
        uint32_t useMoments = 1;
        
        float lengthAttenuation = 0.35f;
        float lengthPower = 1.0f;

        uint32_t finalOutputSrgb = 1;
        uint32_t debugView = 0;

        float lengthSkipThreshold = 0.80f;
        uint32_t enableLengthSkip = 1;
        
        // Motion-confidence shaping for A-Trous history-length protection.
        // motionConfPower <= 0 disables this path, preserving legacy behavior.
        float motionConfPower = 0.0f;
        float motionConfMin = 0.0f;
    };
    static_assert((sizeof(RtAtrousConstants) % 16) == 0, "RtAtrousConstants must be 16-byte aligned.");

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

        float lengthBias = 0.0f;
        float lengthScale = 0.01f;
        float lengthInfluence = 0.5f;
        float motionTrustInfluence = 0.50f;
        float motionConfMin = 0.30f;
        float motionConfPower = 2.0f;
        uint32_t debugView = 0;
        uint32_t  pad0[3] = {};
    };
    static_assert((sizeof(RtHistorySelectConstants) % 16) == 0, "RtHistorySelectConstants must be 16-byte aligned.");

    struct RtMotionDilateConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t radius = 2;
        float depthSigma = 0.02f;

        float normalSigma = 0.25f;
        float minScore = 0.05f;
        uint32_t debugView = 0;
        uint32_t pad0 = 0;
    };

    static_assert((sizeof(RtMotionDilateConstants) % 16) == 0, "RtMotionDilateConstants must be 16-byte aligned.");
    
    enum class RtSignal : uint32_t
    {
        Diffuse,
        Specular,

        // Planned finer splits. Do not allocate these yet.
        DiffuseDirect,
        DiffuseIndirect,
        SpecularDirect,
        SpecularIndirect,

        Emission,
        Transmission,

        NormalRoughness,
        Depth,
        Albedo,
        Motion,
        MaterialId,
        Variance,
        HistoryConfidence,

        BeautyLinear,
        OutputDisplay
    };

    enum class RtPostMode : uint32_t
    {
        Disabled = 0,

        // Production path:
        // accum diffuse/spec
        // -> temporal
        // -> spec history select
        // -> spec A-Trous
        // -> diffuse A-Trous
        // -> final combine
        Full,

        // Diagnostic stop-points.
        RawCombineOnly,
        TemporalOnly,
        HistorySelectOnly,
        SpecAtrousOnly,
        DiffuseAtrousOnly
    };

    struct RtSignalState
    {
        ID3D12Resource* signal = nullptr;
        ID3D12Resource* moments = nullptr;
    };

    enum class RtSpatialFilter : uint32_t
    {
        None = 0,
        Atrous,
        Denoise
    };

    struct RtPostSignals
    {
        RtSignalState diffuse;
        RtSignalState specStable;
        RtSignalState specResponsive;
        RtSignalState specSelected;

        // Final recomposition candidates.
        // If unset, final combine falls back to diffuse/specSelected.
        RtSignalState finalDiffuse;
        RtSignalState finalSpec;

        bool ranDiffuseTemporal = false;
        bool ranSpecStableTemporal = false;
        bool ranSpecResponsiveTemporal = false;
        bool ranHistorySelect = false;

        RtSpatialFilter diffuseSpatial = RtSpatialFilter::None;
        RtSpatialFilter specSpatial = RtSpatialFilter::None;

        bool TemporalReady() const
        {
            return ranDiffuseTemporal && ranSpecStableTemporal;
        }

        bool AdvancedSplitHistoryReady() const
        {
            return ranDiffuseTemporal &&
                ranSpecStableTemporal &&
                ranSpecResponsiveTemporal;
        }

        bool RanDiffuseSpatial() const
        {
            return diffuseSpatial != RtSpatialFilter::None;
        }

        bool RanSpecSpatial() const
        {
            return specSpatial != RtSpatialFilter::None;
        }

        bool RanDiffuseAtrous() const
        {
            return diffuseSpatial == RtSpatialFilter::Atrous;
        }

        bool RanSpecAtrous() const
        {
            return specSpatial == RtSpatialFilter::Atrous;
        }

        bool RanDiffuseDenoise() const
        {
            return diffuseSpatial == RtSpatialFilter::Denoise;
        }

        bool RanSpecDenoise() const
        {
            return specSpatial == RtSpatialFilter::Denoise;
        }

        void SetFinalDiffuse(ID3D12Resource* signal, RtSpatialFilter filter)
        {
            finalDiffuse.signal = signal;
            finalDiffuse.moments = nullptr;
            diffuseSpatial = filter;
        }

        void SetFinalSpec(ID3D12Resource* signal, RtSpatialFilter filter)
        {
            finalSpec.signal = signal;
            finalSpec.moments = nullptr;
            specSpatial = filter;
        }

        ID3D12Resource* FinalDiffuseSignal() const
        {
            return finalDiffuse.signal ? finalDiffuse.signal : diffuse.signal;
        }

        ID3D12Resource* FinalSpecSignal() const
        {
            return finalSpec.signal ? finalSpec.signal : specSelected.signal;
        }
    };

    static bool IsSplitDebug(uint32_t dv)
    {
        return dv >= 48 && dv <= 50;
    }
    
    static bool IsMotionDebug(uint32_t dv)
    {
        return dv == 51 || dv == 52 || dv == 53;
    }

    static bool IsMotionDilateDebug(uint32_t dv)
    {
        return dv == 54 || dv == 55;
    }

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
    void ResetRtAccumulation(bool resetTemporalHistory = true);
    D3D12_CPU_DESCRIPTOR_HANDLE RtUavCpuAt(uint32_t slot) const;

    void CreateRtAovs(ID3D12Device* device, uint32_t width, uint32_t height);
    bool UpdateRtDenoiseSrvTable(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* signalResource,
        DescriptorAllocator::Allocation& table);
    
    void CreateRtHistoryResources(ID3D12Device* device, uint32_t width, uint32_t height);

    bool UpdateRtSvgfSrvTable(
        uint32_t frameIndex,
        uint32_t iter,
        ID3D12Device* device,
        DescriptorAllocator::Allocation& table,
        uint32_t& tableSrvCount,
        ID3D12Resource* signalResource,
        ID3D12Resource* momentsResource,
        ID3D12Resource* motionConfResource);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtAtrousConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        uint32_t iterationIndex,
        bool useMoments,
        bool finalOutputSrgb,
        float motionConfPower,
        float motionConfMin);

    void UpdateRtSvgfPingUavTable(ID3D12Device* device);

    bool UpdateRtTemporalTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* currAccumResource,
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
        float roughnessSigma,
        float motionConfMin,
        float motionConfPower);

    D3D12_GPU_DESCRIPTOR_HANDLE RtSvgfPingUavGpuAt(uint32_t i) const;


    bool UpdateRtHistorySelectTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* stableSignal,
        ID3D12Resource* responsiveSignal,
        ID3D12Resource* stableMoments,
        ID3D12Resource* responsiveMoments);

    void CreateRtPostResources(ID3D12Device* device, uint32_t width, uint32_t height);

    D3D12_CPU_DESCRIPTOR_HANDLE RtPostUavCpuAt(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE RtPostUavGpuAt(uint32_t index) const;


    bool UpdateRtCombineSrvTable(uint32_t frameIndex, ID3D12Device* device, 
        ID3D12Resource* diffuseResource, ID3D12Resource* specularResource);

    static bool RtPostModeRunsTemporal(RtPostMode mode);
    static bool RtPostModeRunsHistorySelect(RtPostMode mode);
    static bool RtPostModeRunsSpecAtrous(RtPostMode mode);
    static bool RtPostModeRunsDiffuseAtrous(RtPostMode mode);
    static bool RtPostModeRunsAnyAtrous(RtPostMode mode);
    static bool RtPostModeCommitsHistory(RtPostMode mode);

    bool CombineRtSignalsToOutput(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Resource* diffuseResource,
        ID3D12Resource* specularResource,
        uint32_t width,
        uint32_t height,
        const char* markerName);

    bool RunRtRawCombineOnly(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

    bool RunRtTemporalOnlyCombine(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Resource* diffuseSignal,
        ID3D12Resource* specStableSignal,
        uint32_t width,
        uint32_t height);

    bool RunRtHistorySelectOnlyCombine(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Resource* diffuseSignal,
        ID3D12Resource* specSelectedSignal,
        uint32_t width,
        uint32_t height);

    bool RunRtSpecAtrousOnlyCombine(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Resource* diffuseSignal,
        uint32_t width,
        uint32_t height);

    bool RunRtDiffuseAtrousOnlyCombine(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Resource* specStableSignal,
        uint32_t width,
        uint32_t height);

    bool RunRtFinalCombine(
        ID3D12Device* device,
        CommandList& cl,
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

    bool RunRtDenoiseSignal(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        const char* eventName,
        ID3D12Resource* inputSignal,
        DescriptorAllocator::Allocation& srvTable,
        ID3D12Resource* outputResource,
        D3D12_GPU_DESCRIPTOR_HANDLE outputUav,
        uint32_t width,
        uint32_t height);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtRayGenConstants(uint32_t frameIndex);
    void CommitRtMotionWorlds();

    bool UpdateRtMotionDilateTables(
        uint32_t frameIndex,
        ID3D12Device* device);

    bool RunRtMotionDilate(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtMotionDilateConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

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
    // -----------------------------------------------------------------------------
    // RT DebugView registry - locked namespaces
    //
    // Core RT / raygen-style views:
    //   0  = final shaded output
    //   27 = stored guide roughness from g_AovNormal.a
    //
    // Temporal pass owns:
    //   18 = reprojection validity
    //   19 = rejection / disocclusion
    //   20 = chosen previous UV
    //   21 = history-current difference
    //   22 = history length
    //   23 = temporal alpha
    //   24 = temporal moments variance
    //   25 = history warm-up / convergence factor
    //   26 = depth reprojection error
    //   32 = spec-direction reuse mask
    //   33 = spec-direction dot
    //   34 = reprojection search chosen offset
    //   35 = reprojection best score
    //   36 = confidence-scaled alpha
    //   45 = variance-normalized signal
    //   46 = reprojection best score
    //   47 = final alpha after variance shaping
    //
    // History-select pass owns:
    //   29 = final history selector mask
    //   30 = stable history signal
    //   31 = responsive history signal
    //   37 = roughness selector vote
    //   38 = length selector vote
    //   39 = final selector value
    //   40 = stable history length
    //   41 = responsive history length
    //   42 = selected history length
    //   59 = selected spec variance
    //
    // SVGF / A-Trous pass owns:
    //   28 = roughness/specular protection proxy
    //   43 = center-history length attenuation factor
    //   44 = wide-iteration skip mask
    //   60 = spec A-Trous shaped motion confidence
    //
    // Split / RayGen-owned views:
    // These write directly to m_rtOutput from RayGen.
    // The RT post stack must stay disabled so temporal/SVGF/combine cannot overwrite them.
    //   48 = diffuse accumulation
    //   49 = specular accumulation
    //   50 = diffuse + specular accumulation
    // 
    // Motion / RayGen-owned views:
    // These write directly to m_rtOutput from RayGen.
    // The RT post stack must stay disabled.
    //   51 = stored previous UV visualization
    //   52 = invalid previous UV mask
    //   53 = raw previous UV validity mask, white = invalid
    //
    // Motion dilation / compute-owned views:
    // This is produced by RtMotionDilatePass and writes directly to m_rtOutput.
    // The RT post stack must stay disabled, but RunRtMotionDilate still runs explicitly.
    //   54 = dilated previous UV validity mask, white = invalid
    //   55 = motion confidence, white = high confidence
    // 
    // Future rule:
    //   Do not use broad contiguous checks such as 32..47.
    //   Always route by the owning pass namespace.
    // -----------------------------------------------------------------------------
    //
    // RT post-stack validation checklist after future tuning:
    //   Temporal:
    //     45 = varNorm bright in noisy regions
    //     46 = reprojection score behaves smoothly
    //     47 = final alpha increases when variance boost is enabled
    //     56 = motion confidence consumed by temporal
    //     57 = alpha after motion-confidence scaling
    //     58 = post-power motion confidence used for temporal weighting
    //   History select:
    //     42 = selectedLen looks plausible and stable
    //   A-Trous:
    //     43 = attenuation darkens where selectedLen is high
    //     44 = skip mask responds to threshold after convergence
    //   Integration:
    //     40/41/42/43/44 must not be captured by temporal debug output.
    // -----------------------------------------------------------------------------
    uint32_t m_debugView = 0;
    bool  m_autoOrbit = true;
    bool m_pauseAnimation = false;
    bool m_useRaytracing = false;        // Toggle for raytracing vs rasterization (for testing/debugging)
    bool m_rtAccumulate = false;         // validation / progressive mode
    bool m_rtSvgf = true;
    bool  m_rtDenoise = true;

    // RT post-stack mode.
    // Full is the production path.
    // Diagnostic modes stop the post stack at known signal boundaries and combine
    // the current diffuse/spec pair into m_rtOutput for inspection.
    bool m_rtEnablePostStack = true;
    RtPostMode m_rtPostMode = RtPostMode::Full;

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

        DescriptorAllocator::Allocation temporalDiffuseSrvTable{};     // 9 SRVs
        DescriptorAllocator::Allocation temporalDiffuseUavTable{};     // 3 UAVs

        DescriptorAllocator::Allocation temporalSpecStableSrvTable{};  // 9 SRVs
        DescriptorAllocator::Allocation temporalSpecStableUavTable{};  // 3 UAVs

        DescriptorAllocator::Allocation temporalSpecRespSrvTable{};    // 9 SRVs
        DescriptorAllocator::Allocation temporalSpecRespUavTable{};    // 3 UAVs

        DescriptorAllocator::Allocation historySelectSrvTable{};  // kRtHistorySelectSrvCount SRVs
        DescriptorAllocator::Allocation historySelectUavTable{};  // kRtHistorySelectUavCount UAVs
        uint32_t historySelectSrvCount = 0;
        uint32_t historySelectUavCount = 0;
        DescriptorAllocator::Allocation denoiseDiffuseSrvTable{}; // 3 SRVs
        DescriptorAllocator::Allocation denoiseSpecSrvTable{};    // 3 SRVs
        DescriptorAllocator::Allocation combineSrvTable{};             // 2 SRVs

        // Per-frame, per-iteration SVGF input tables.
        std::array<DescriptorAllocator::Allocation, kMaxRtAtrousIterations> svgfSpecSrvTables{};
        std::array<DescriptorAllocator::Allocation, kMaxRtAtrousIterations> svgfDiffuseSrvTables{};

        std::array<uint32_t, kMaxRtAtrousIterations> svgfSpecSrvCounts{};
        std::array<uint32_t, kMaxRtAtrousIterations> svgfDiffuseSrvCounts{};

        DescriptorAllocator::Allocation motionDilateSrvTable{}; // 3 SRVs
        DescriptorAllocator::Allocation motionDilateUavTable{}; // 3 UAVs

        uint32_t capacity = 0;
    };

    
    std::vector<FrameRaytracingResources> m_rtFrames;
    uint32_t m_frameCount = 0;

    RaytracingPipeline m_rtPipeline;

    // RT output
    ComPtr<ID3D12Resource> m_rtOutput;
    // Contiguous DXR UAV table:
    // u0 = m_rtOutput          R8G8B8A8_UNORM display
    // u1 = m_rtAccumDiffuse    R16G16B16A16_FLOAT linear
    // u2 = m_rtAccumSpec       R16G16B16A16_FLOAT linear
    // u3 = m_rtAovNormal       R16G16B16A16_FLOAT rgb=geom normal, a=roughness
    // u4 = m_rtAovDepth        R32_FLOAT
    // u5 = m_rtAovMotion       R16G16_FLOAT prevUV, (-1,-1) invalid
    DescriptorAllocator::Allocation m_rtOutputUav{};
    uint32_t m_rtOutputWidth = 0;
    uint32_t m_rtOutputHeight = 0;


    uint32_t m_rtMaterialCount = 4;
    bool m_rtOutputReady = false;
    uint32_t m_rtSampleIndex = 0;
    uint32_t m_rtDispatchSampleIndex = 0;
    uint32_t m_rtResetId = 0;
    bool m_rtAccumulateThisFrame = false;
    bool m_rtAccumulatingLastFrame = false;

    ComPtr<ID3D12Resource> m_rtAccumDiffuse;
    ComPtr<ID3D12Resource> m_rtAccumSpec;

    bool m_rtAccumDiffuseReady = false;
    bool m_rtAccumSpecReady = false;

    bool m_rtHistoryValid = false;
    uint32_t m_prevRtDebugView = 0;
    float m_prevRtCamYaw = 0.0f;
    float m_prevRtCamPitch = 0.0f;
    float m_prevRtCamRadius = 0.0f;
    std::vector<DirectX::XMFLOAT4X4> m_prevRtWorlds;
    std::vector<const Material*> m_prevRtMaterials;
    std::vector<DirectX::XMFLOAT4X4> m_prevRtMotionWorlds;
    bool m_prevRtMotionWorldsValid = false;

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

    ComPtr<ID3D12Resource> m_rtAovMotion;
    bool m_rtAovMotionReady = false;

    ComPtr<ID3D12Resource> m_rtAovMotionDilated;
    bool m_rtAovMotionDilatedReady = false;
    ComPtr<ID3D12Resource> m_rtAovMotionConf;
    bool m_rtAovMotionConfReady = false;
    

    bool m_rtAovReady = false;

    //DescriptorAllocator::Allocation m_rtDenoiseSrvTable{};
    RtDenoisePass m_rtDenoisePass;


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


    uint32_t m_rtAtrousIterations = 2;
    float m_rtVarianceScale = 1.0f;

    bool m_prevRtSvgf = true;
    uint32_t m_prevRtAtrousIterations = 2;
    float m_prevRtVarianceScale = 1.0f;

    float m_rtAtrousLengthAttenuation = 0.35f;
    float m_rtAtrousLengthPower = 1.0f;

    float m_prevRtAtrousLengthAttenuation = 0.35f;
    float m_prevRtAtrousLengthPower = 1.0f;

    float    m_rtAtrousLengthSkipThreshold = 0.80f;
    uint32_t m_rtAtrousEnableLengthSkip = 1;

    float    m_prevRtAtrousLengthSkipThreshold = 0.80f;
    uint32_t m_prevRtAtrousEnableLengthSkip = 1;

    DescriptorAllocator::Allocation m_rtSvgfPingUavTable{};

    float m_rtTemporalRoughnessSigma = 0.15f;
    float m_prevRtTemporalRoughnessSigma = 0.15f;

    RtHistorySelectPass m_rtHistorySelectPass;

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
    // Motion confidence policy per signal.
    float m_rtTemporalMotionConfMinDiffuse = 0.15f;
    float m_rtTemporalMotionConfMinSpec = 0.30f;

    float m_rtTemporalMotionConfPowerDiffuse = 1.0f;
    float m_rtTemporalMotionConfPowerSpec = 2.0f;

    uint32_t m_prevRtTemporalReprojectRadius = 1;
    float    m_prevRtTemporalReprojectMinConf = 0.25f;
    float m_prevRtTemporalMotionConfMinDiffuse = 0.15f;
    float m_prevRtTemporalMotionConfMinSpec = 0.30f;
    float m_prevRtTemporalMotionConfPowerDiffuse = 1.0f;
    float m_prevRtTemporalMotionConfPowerSpec = 2.0f;
    float m_rtHistorySelectLengthBias = 0.0f;

    // Keep this small. History lengths are 0 - 255;
    // high values make the length vote saturate quickly.
    float m_rtHistorySelectLengthScale = 0.01f;

    // Motion confidence influence for spec history selection.
    // 0 = selector ignores motion confidence.
    // 1 = low confidence strongly pushes toward responsive history.
    float m_rtHistorySelectMotionTrustInfluence = 0.50f;

    float m_prevRtHistorySelectLengthBias = 0.0f;
    float m_prevRtHistorySelectLengthScale = 0.01f;

    float m_rtHistorySelectLengthInfluence = 0.5f;
    float m_prevRtHistorySelectLengthInfluence = 0.5f;
    float m_prevRtHistorySelectMotionTrustInfluence = 0.50f;

    float    m_rtTemporalVarianceScale = 16.0f;
    float    m_rtTemporalVarianceBias = 0.0f;
    float    m_rtTemporalVarianceAlphaBoost = 0.5f;
    uint32_t m_rtTemporalEnableVarianceBoost = 1;

    float    m_prevRtTemporalVarianceScale = 16.0f;
    float    m_prevRtTemporalVarianceBias = 0.0f;
    float    m_prevRtTemporalVarianceAlphaBoost = 0.5f;
    uint32_t m_prevRtTemporalEnableVarianceBoost = 1;

    ComPtr<ID3D12Resource> m_rtPostDiffuse;
    ComPtr<ID3D12Resource> m_rtPostSpec;
    bool m_rtPostReady = false;

    DescriptorAllocator::Allocation m_rtPostUavTable{};
    
    // Moments corresponding to the selected spec signal after RtHistorySelectPass.
    // Format: R16G16_FLOAT, xy = mean luminance / mean-square luminance.
    ComPtr<ID3D12Resource> m_rtSpecSelectedMoments;
    bool m_rtSpecSelectedMomentsReady = false;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistorySpec{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistorySpecResp{};

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryMomentsSpec{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryMomentsSpecResp{};

    RtCombinePass m_rtCombinePass;

    uint32_t m_rtAtrousIterationsSpec = 1;
    uint32_t m_prevRtAtrousIterationsSpec = 1;

    static constexpr uint32_t kRtUavTableCount = 6;
    static constexpr uint32_t kRtHistorySelectSrvCount = 7;
    static constexpr uint32_t kRtHistorySelectUavCount = 3;
    static constexpr uint32_t kRtSvgfSrvCount = 5;

    RtMotionDilatePass m_rtMotionDilatePass;
    static constexpr uint32_t kMaxRtMotionDilateRadius = 4;
    uint32_t m_rtMotionDilateRadius = 2;
    float m_rtMotionDilateDepthSigma = 0.02f;
    float m_rtMotionDilateNormalSigma = 0.25f;
    float m_rtMotionDilateMinScore = 0.05f;

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtHistorySelectConstants(uint32_t frameIndex);
};
 
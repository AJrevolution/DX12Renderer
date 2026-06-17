#pragma once
#include <filesystem>
#include <vector>
#include <array>
#include <cstddef>
#include <cstdint>
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
#include "Source/Renderer/Passes/RtHitDistReconstructPass.h"
#include "Source/Renderer/Passes/RtDiffuseDemodulatePass.h"
#include "Source/Renderer/Passes/RtOutlierClampPass.h"
#include "Source/Renderer/RtEnvironmentImportance.h"
#include "Source/Renderer/RtEnvironmentMapCpu.h"
#include "Source/Renderer/Passes/RtRestirTemporalPass.h"
#include "Source/Renderer/Passes/RtRestirSpatialPass.h"
#include "Source/Renderer/Passes/RtRestirApplyPass.h"

enum class DebugViewDomain : uint8_t
{
    Final,
    Raster,
    GBuffer,
    Shadow,
    DXR,
    RT_AOV,
    RT_Temporal,
    RT_HistorySelect,
    RT_Spatial,
    RT_GuideReconstruct,
    RT_Combine,
    RT_Sampling,
    RT_ReSTIR
};

enum class DebugViewAvailability : uint8_t
{
    Available,
    Unavailable,
    RequiresDxrSupport,
    RequiresRaytracingEnabled,
    RequiresDxrPipeline,
    PendingResources,
    PendingHistory,
    RequiresRestir,
    RequiresGBuffer,
    RequiresShadow,
    UnknownId
};

enum DebugViewRequirementFlags : uint32_t
{
    DebugViewReq_None = 0,
    DebugViewReq_DxrSupport = 1u << 0,
    DebugViewReq_RaytracingEnabled = 1u << 1,
    DebugViewReq_DxrPipeline = 1u << 2,
    DebugViewReq_RtResources = 1u << 3,
    DebugViewReq_RtAovs = 1u << 4,
    DebugViewReq_RtHistory = 1u << 5,
    DebugViewReq_RtPostStack = 1u << 6,
    DebugViewReq_Restir = 1u << 7,
    DebugViewReq_GBuffer = 1u << 8,
    DebugViewReq_Shadow = 1u << 9
};

struct DebugViewDesc
{
    uint32_t id = 0;
    const char* name = "";
    const char* category = "";

    DebugViewDomain domain = DebugViewDomain::Final;
    uint32_t requirements = DebugViewReq_None;

    // True for IDs consumed by the existing RT debug routing system.
    bool affectsRtRouting = false;

    // Metadata only in this first patch. RenderFrame still preserves the
    // existing debug-view-change reset behaviour.
    bool canResetAccumulation = false;
    bool canResetTemporalHistory = false;

    // True when the existing routing can make this ID own the displayed
    // m_rtOutput image for the frame.
    bool canOwnFinalRtOutput = false;
};

const DebugViewDesc* FindDebugViewDesc(uint32_t id);
const DebugViewDesc* GetDebugViewDescs(std::size_t& count);
const char* DebugViewAvailabilityName(DebugViewAvailability availability);

enum class CameraControlMode : uint8_t
{
    AutoOrbit,
    ManualOrbit,
    FreeRoam
};

struct OrbitCameraInput
{
    // -1..1 style axes supplied by the application input layer.
    // Renderer owns speed, clamping, and camera state changes.
    float yawAxis = 0.0f;
    float zoomAxis = 0.0f;
    float deltaSeconds = 0.0f;
};

struct FreeRoamCameraInput
{
    // -1..1 style axes supplied by the application input layer.
    // Mouse deltas are in pixels accumulated by Window since last frame.
    float moveForwardAxis = 0.0f;
    float moveRightAxis = 0.0f;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float deltaSeconds = 0.0f;
};

const char* CameraControlModeName(CameraControlMode mode);

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

    DebugViewAvailability GetDebugViewAvailability(uint32_t id) const;
    bool IsDebugViewSelectable(uint32_t id) const;
    bool SetDebugView(uint32_t id);
    uint32_t GetDebugView() const;
    bool IsRaytracingEnabled() const;
    void SetRaytracingEnabled(bool enabled);
    CameraControlMode GetCameraControlMode() const;
    void SetCameraControlMode(CameraControlMode mode);
    bool IsAutoOrbitEnabled() const;
    void SetAutoOrbitEnabled(bool enabled);
    void ToggleAutoOrbit();
    bool IsFreeRoamEnabled() const;
    void SetFreeRoamEnabled(bool enabled);
    void ToggleFreeRoam();
    void ApplyOrbitCameraInput(const OrbitCameraInput& input);
    void ApplyFreeRoamCameraInput(const FreeRoamCameraInput& input);

private:

    void ComputeOrbitCamera(
        DirectX::XMFLOAT3& outPosition,
        DirectX::XMFLOAT3& outTarget) const;

    void InitialiseFreeRoamFromOrbitCamera();
    void ProjectOrbitCameraFromFreeRoam();

    enum class RtEnvSamplingMode : uint32_t
    {
        BrdfOnly = 0,
        EnvOnly = 1,
        MisOneSample = 2,
        MisTwoSampleReference = 3
    };

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

        float viewZSigmaScale = 0.0f;
        float viewZRoughCutoff = 0.35f;
        float viewZConfMin = 0.5f;
        float pad1 = 0.0f;

        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;

        uint32_t surfaceIdHistoryValid = 0;
        uint32_t padSurfaceId[3] = {};

        float varianceScale = 16.0f;
        float varianceBias = 0.0f;
        float varianceAlphaBoost = 0.5f;
        uint32_t enableVarianceBoost = 1;

        DirectX::XMFLOAT4 currCameraPos{};
        DirectX::XMFLOAT4 prevCameraPos{};

        uint32_t enableRobustMoments = 1;
        float momentLuminanceMax = 250.0f;
        float momentVarianceMax = 250.0f * 250.0f;
        float historyClampStrength = 0.5f;

        float temporalNeighborhoodSigmaK = 4.0f;
        float temporalClampMinWeight = 1.0f;
        float temporalClampRelaxation = 0.5f;
        float padRobust0 = 0.0f;

        uint32_t enableSignalConfidence = 1;
        float signalDeltaSigma = 1.5f;
        float confidencePower = 1.0f;
        float minSignalConfidence = 0.05f;

        float antiLagStrength = 0.35f;
        float varianceConfidenceScale = 2.0f;
        float historyLengthConfidencePower = 0.75f;
        float responsiveAlphaBoost = 2.0f;

        float maxStableHistory = 255.0f;
        float minStableHistoryForClamp = 2.0f;
        float confidenceDebugScale = 1.0f;
        float padShape0 = 0.0f;
    };
    static_assert((sizeof(RtTemporalConstants) % 16) == 0, "RtTemporalConstants must be 16-byte aligned.");

    struct RtSamplingConstants
    {
        uint32_t envSamplingMode = 0; // BrdfOnly
        uint32_t useEnvImportanceSampling = 1;
        uint32_t useEnvMIS = 0;
        uint32_t envAliasCount = 0;

        uint32_t envFaceSize = 0;
        uint32_t envAliasFallback = 1;
        uint32_t samplingDebugView = 0;
        uint32_t useEnvNeeForFinal = 0;

        float envIntensity = 1.0f;
        float envPdfEpsilon = 1e-6f;
        float envDeltaRoughnessCutoff = 0.04f;
        float envMISPower = 2.0f;

        uint32_t envNeeFireflyGuard = 1;
        uint32_t envAliasVersion = 0;
        float envNeeMaxRadiance = 20.0f;
        float pad1 = 0.0f;
    };
    static_assert((sizeof(RtSamplingConstants) % 16) == 0, "RtSamplingConstants must be 16-byte aligned.");

    struct RtRestirConstants
    {
        uint32_t enableRestirEnvDi = 0;
        uint32_t restirInitialCandidateCount = 1;
        uint32_t restirDebugView = 0;
        uint32_t restirDispatchMode = 0; // 0 = normal trace, 1 = resolve

        float restirMaxM = 32.0f;
        float restirMaxAge = 32.0f;
        float restirMinTarget = 1e-5f;
        float restirMaxWeight = 64.0f;
    };
    static_assert((sizeof(RtRestirConstants) % 16) == 0, "RtRestirConstants must be 16-byte aligned.");

    struct RtRayGenConstants
    {
        DirectX::XMFLOAT4X4 prevViewProj{};
        DirectX::XMFLOAT4X4 currViewProj{};
        DirectX::XMFLOAT4 prevCameraPos{};
        DirectX::XMFLOAT4 currCameraPos{};

        uint32_t hasPrevMotion = 0;
        uint32_t pad0[3] = {};

        RtSamplingConstants sampling{};
        RtRestirConstants restir{};
    };
    static_assert((sizeof(RtRayGenConstants) % 16) == 0, "RtRayGenConstants must be 16-byte aligned.");

    struct RtRestirTemporalConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t temporalEnabled = 1;
        uint32_t historyValid = 0;

        uint32_t surfaceIdHistoryValid = 0;
        uint32_t viewZHistoryValid = 0;
        uint32_t debugView = 0;
        uint32_t frameIndex = 0;

        float depthSigma = 0.02f;
        float normalSigma = 0.25f;
        float roughnessSigma = 0.20f;
        float viewZSigma = 0.08f;

        float reprojectMinWeight = 0.25f;
        float maxM = 32.0f;
        float maxAge = 32.0f;
        float maxWeight = 64.0f;
    };
    static_assert((sizeof(RtRestirTemporalConstants) % 16) == 0, "RtRestirTemporalConstants must be 16-byte aligned.");
    
    struct RtRestirSpatialConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t sampleCount = 4;
        uint32_t radius = 8;

        float normalSigma = 0.25f;
        float depthSigma = 0.02f;
        float roughnessSigma = 0.20f;
        float viewZSigma = 0.08f;

        float maxM = 32.0f;
        float maxWeight = 64.0f;
        uint32_t frameIndex = 0;
        uint32_t debugView = 0;

        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;
    };
    static_assert((sizeof(RtRestirSpatialConstants) % 16) == 0, "RtRestirSpatialConstants must be 16-byte aligned.");

    struct RtRestirApplyConstants
    {
        float diffuseScale = 0.25f;
        float specularScale = 0.25f;
        uint32_t mode = 1;  // 0 = disabled, 1 = validation additive
        uint32_t flags = 0;
    };
    static_assert((sizeof(RtRestirApplyConstants) % 16) == 0, "RtRestirApplyConstants must be 16-byte aligned.");

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

        float viewZSigmaScale = 0.0f;
        float viewZRoughCutoff = 0.35f;

        float viewZConfMin = 0.5f;
        float pad = 0.0f;

        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;

        float atrousContributionMaxLuminance = 250.0f;
        float padSafety[3] = {};

        uint32_t enableAdaptiveAtrous = 1;
        float adaptiveVarianceScale = 1.0f;
        float adaptiveHistoryMin = 1.0f;
        float adaptiveHistoryMax = 16.0f;

        float diffuseBlurBoost = 1.0f;
        float specBlurRoughnessBoost = 0.75f;
        float specGlossyBlurLimit = 0.35f;
        float wideIterationConfidenceMin = 0.35f;

        float adaptiveSigmaLMin = 0.5f;
        float adaptiveSigmaLMax = 2.0f;
        float adaptiveNormalRelaxation = 0.25f;
        float padAtrousShape0 = 0.0f;
    };
    static_assert((sizeof(RtAtrousConstants) % 16) == 0, "RtAtrousConstants must be 16-byte aligned.");

    struct DrawItem
    {
        Mesh* mesh = nullptr;
        Material* material = nullptr;
        DirectX::XMFLOAT4X4 world{};
        uint32_t rtObjectId = 0;
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

        uint32_t enableSpecHistoryShaping = 1;
        float specResponsiveRoughnessCutoff = 0.35f;
        float specResponsiveVarianceScale = 2.0f;
        float specResponsiveMotionScale = 1.0f;

        float specStableMinHistory = 8.0f;
        float specResponsiveBias = 0.0f;
        float specHistoryBlendPower = 1.0f;
        float padSpecShape0 = 0.0f;
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
        
        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;
    };
    static_assert((sizeof(RtMotionDilateConstants) % 16) == 0, "RtMotionDilateConstants must be 16-byte aligned.");
    
    struct RtOutlierClampConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        uint32_t radius = 1;
        uint32_t signalKind = 0;

        float depthSigma = 0.02f;
        float normalSigma = 0.25f;
        float roughnessSigma = 0.20f;
        float padGuide0 = 0.0f;

        float sigmaK = 4.0f;
        float maxLuminance = 250.0f;
        float minNeighborhoodWeight = 1.0f;
        float minClampLuminance = 0.0f;

        float surfaceIdRequired = 1.0f;
        float clampStrength = 1.0f;
        float motionRelaxation = 0.5f;
        uint32_t debugView = 0;

        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;

        uint32_t useViewZ = 0;
        uint32_t useMotionConf = 0;
        uint32_t pad0[2] = {};
    };
    static_assert((sizeof(RtOutlierClampConstants) % 16) == 0, "RtOutlierClampConstants must be 16-byte aligned.");

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

    struct IRtGuideReconstruction
    {
        virtual ~IRtGuideReconstruction() = default;
    };

    struct IRtTemporalFilter
    {
        virtual ~IRtTemporalFilter() = default;
    };

    struct IRtSpatialFilter
    {
        virtual ~IRtSpatialFilter() = default;
    };  

    struct RtViewZReconstructConstants
    {
        DirectX::XMFLOAT2 invResolution{};
        float alpha = 0.20f;
        float depthSigma = 0.02f;

        float normalSigma = 0.25f;
        float roughnessSigma = 0.20f;
        uint32_t historyValid = 0;
        uint32_t debugView = 0;

        uint32_t radius = 2;
        float viewZVisMax = 25.0f;
        uint32_t pad0[2] = {};

        DirectX::XMFLOAT3 distanceNormParams{ 1.0f, 0.0f, 1.0f };
        float distanceNormSigma = 0.08f;
    };
    static_assert((sizeof(RtViewZReconstructConstants) % 16) == 0, "RtViewZReconstructConstants must be 16-byte aligned.");

    struct RtDiffuseDemodulateConstants
    {
        uint32_t debugView = 0;
        uint32_t pad0[3] = {};
    };
    static_assert((sizeof(RtDiffuseDemodulateConstants) % 16) == 0, "RtDiffuseDemodulateConstants must be 16-byte aligned.");

    struct RtCombineConstants
    {
        uint32_t diffuseIsDemodulated = 0;
        uint32_t pad0[3] = {};
    };
    static_assert((sizeof(RtCombineConstants) % 16) == 0, "RtCombineConstants must be 16-byte aligned.");

    struct RtRestirReservoir
    {
        DirectX::XMFLOAT4 sampleDir_pdf{};
        DirectX::XMFLOAT4 sampleLi_target{};
        DirectX::XMFLOAT4 weightSum_M_W{};
        uint32_t sampleIndex = 0;
        uint32_t flags = 0;
        uint32_t age = 0;
        uint32_t surfaceId = 0xFFFFFFFFu;
    };
    static_assert(sizeof(RtRestirReservoir) == 64, "RtRestirReservoir must match HLSL layout.");

    // Denoiser shaping contract:
    //
    // SurfaceId:
    //   Hard identity gate. Never overridden by confidence.
    //
    // ViewZ:
    //   Normalized distance guide. Used only as a similarity/shaping weight.
    //
    // Motion confidence:
    //   Reprojection confidence from motion dilation. Low confidence accelerates temporal response.
    //
    // Robust moments:
    //   Bounded luminance moments from 10.4. Used for variance-aware alpha and A-Trous shaping.
    //
    // History length:
    //   Stored in signal alpha. Represents usable accumulated frames after confidence shaping.
    //
    // Signal confidence:
    //   Derived per temporal pass from guide match, motion confidence, luminance delta,
    //   variance, robust clamp state, and history validity.
    //   0 = reset / highly responsive.
    //   1 = stable reuse.
    //
    // Spatial blur strength:
    //   Derived in A-Trous from moments, roughness, motion confidence, and history length.
    //   Low history / high variance => more diffuse blur.
    //   Glossy specular => stricter, smaller blur unless confidence is very low.
    
    // -------------------------------------------------------------------------
    // RT denoiser data contracts
    // -------------------------------------------------------------------------

    struct RtDenoiserSignal
    {
        ID3D12Resource* signal = nullptr;   // R16G16B16A16_FLOAT, linear HDR.
        ID3D12Resource* moments = nullptr;  // R16G16_FLOAT moments, optional.

        explicit operator bool() const
        {
            return signal != nullptr;
        }
    };

    struct RtDenoiserSignals
    {      
        RtDenoiserSignal diffuse{};
        RtDenoiserSignal specStable{};
        RtDenoiserSignal specResponsive{};
        RtDenoiserSignal specSelected{};

        RtDenoiserSignal diffuseFinal{};
        RtDenoiserSignal specFinal{};

        bool TemporalReady() const
        {
            return diffuse.signal &&
                specStable.signal &&
                diffuse.moments &&
                specStable.moments;
        }

        bool AdvancedSplitHistoryReady() const
        {
            return diffuse.signal &&
                specStable.signal &&
                specResponsive.signal &&
                diffuse.moments &&
                specStable.moments &&
                specResponsive.moments;
        }

        ID3D12Resource* FinalDiffuseSignal() const
        {
            return diffuseFinal.signal ? diffuseFinal.signal : diffuse.signal;
        }

        ID3D12Resource* FinalSpecSignal() const
        {
            return specFinal.signal ? specFinal.signal : specSelected.signal;
        }
    };

    struct RtDenoiserGuides
    {
        ID3D12Resource* normalRough = nullptr;          // R16G16B16A16
        ID3D12Resource* depth = nullptr;                // R32
        ID3D12Resource* prevUVRaw = nullptr;            // R16G16, DXR u5
        ID3D12Resource* prevUVDilated = nullptr;        // R16G16, compute
        ID3D12Resource* motionConf = nullptr;           // R16, compute

        ID3D12Resource* viewZRaw = nullptr;    // R16, DXR u6
        ID3D12Resource* viewZRecons = nullptr;        // R16, compute
        ID3D12Resource* viewZReconsConf = nullptr;    // R16, compute

        ID3D12Resource* viewZHistoryRead = nullptr;        // R16, previous reconstructed viewZ
        ID3D12Resource* viewZConfHistoryRead = nullptr;    // R16, previous reconstructed confidence

        ID3D12Resource* surfaceId = nullptr;            // R32_UINT
        ID3D12Resource* diffuseAlbedo = nullptr;        // R16G16B16A16, a=stable demod flag

        ID3D12Resource* diffuseDemodulated = nullptr;   // R16G16B16A16, diffuse lighting

        bool ReadyForViewZReconstruct() const
        {
            return viewZRaw &&
                normalRough &&
                depth &&
                prevUVRaw &&
                surfaceId &&
                viewZRecons &&
                viewZReconsConf;
        }

        bool ReadyForMotionDilate() const
        {
            return prevUVRaw &&
                prevUVDilated &&
                motionConf &&
                normalRough &&
                depth &&
                viewZRaw &&
                surfaceId;
        }

        bool ReadyForDiffuseDemodulate() const
        {
            return diffuseAlbedo &&
                diffuseDemodulated &&
                depth;
        }

        bool ReadyForTemporal() const
        {
            return normalRough &&
                depth &&
                prevUVDilated &&
                motionConf &&
                diffuseAlbedo &&
                diffuseDemodulated;
        }

        bool ReadyForSpatial(bool needsMotionConfidence) const
        {
            return normalRough &&
                depth &&
                diffuseAlbedo &&
                diffuseDemodulated &&
                surfaceId &&
                (!needsMotionConfidence || motionConf);
        }

        bool ReadyForCombine() const
        {
            return diffuseAlbedo != nullptr;
        }

        bool ReadyForViewZTemporal() const
        {
            return viewZRecons &&
                viewZHistoryRead &&
                viewZConfHistoryRead;
        }

        bool ReadyForViewZSpatial() const
        {
            return viewZRecons &&
                viewZReconsConf;
        }
    };

    struct RtDenoiserHistories
    {
        RtDenoiserSignal diffuseRead{};
        RtDenoiserSignal diffuseWrite{};

        RtDenoiserSignal specStableRead{};
        RtDenoiserSignal specStableWrite{};

        RtDenoiserSignal specResponsiveRead{};
        RtDenoiserSignal specResponsiveWrite{};

        ID3D12Resource* normalRead = nullptr;
        ID3D12Resource* normalWrite = nullptr;

        ID3D12Resource* depthRead = nullptr;
        ID3D12Resource* depthWrite = nullptr;

        ID3D12Resource* viewZRead = nullptr;
        ID3D12Resource* viewZWrite = nullptr;

        ID3D12Resource* viewZConfRead = nullptr;
        ID3D12Resource* viewZConfWrite = nullptr;

        ID3D12Resource* surfaceIdRead = nullptr;
        ID3D12Resource* surfaceIdWrite = nullptr;
    };

    enum class RtDebugOwner : uint8_t
    {
        None,
        RayGen,
        GuideReconstruct,
        Temporal,
        HistorySelect,
        Spatial,
        Combine
    };

    struct RtDebugRouting
    {
        uint32_t debugView = 0;

        bool wantsTemporalDebug = false;
        bool wantsSvgfDebug = false;
        bool wantsAtrousOutputDebug = false;
        bool wantsSpecAtrousOutputDebug = false;
        bool wantsHistorySelectDebug = false;

        bool wantsSplitDebug = false;
        bool wantsMotionDebug = false;
        bool wantsMotionDilateDebug = false;
        bool wantsViewZDebug = false;
        bool wantsViewZReconstructDebug = false;
        bool wantsSurfaceIdDebug = false;
        bool wantsDiffuseAlbedoDebug = false;
        bool wantsDiffuseDemodDebug = false;

        bool wantsRtPostDebug = false;
        bool wantsRtInspectionDebug = false;
        bool wantsProducerDebug = false;
        bool wantsSpatialDebug = false;

        bool wantsOutlierClampDebug = false;
        bool wantsRtSamplingDebug = false;

        bool wantsRtRestirDebug = false;
        bool wantsRtRestirRayGenDebug = false;
        bool wantsRtRestirTemporalDebug = false;
        bool wantsRtRestirSpatialDebug = false;
        bool wantsRtRestirResolveDebug = false;

        RtDebugOwner owner = RtDebugOwner::None;

        bool DisablesPostStack() const
        {
            return wantsProducerDebug;
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

    static bool IsViewZDebug(uint32_t dv)
    {
        return dv == 61 || dv == 62;
    }

    static bool IsViewZReconstructDebug(uint32_t dv)
    {
        return dv == 63 || dv == 64 ||
            dv == 79 || dv == 80;
    }

    static bool IsSurfaceIdDebug(uint32_t dv)
    {
        return dv == 65 || dv == 66 ||
            dv == 74 || dv == 75;
    }

    static bool IsDiffuseAlbedoDebug(uint32_t dv)
    {
        return dv == 67 || dv == 68;
    }

    static bool IsDiffuseDemodulateDebug(uint32_t dv)
    {
        return dv == 69 || dv == 70;
    }

    static DirectX::XMFLOAT3 RtDistanceNormParams()
    {
        return DirectX::XMFLOAT3(1.0f, 0.0f, 1.0f);
    }

    static bool IsOutlierClampDebug(uint32_t dv)
    {
        return dv == 81 || dv == 82 || dv == 83 || dv == 86;
    }

    static bool IsRtSamplingDebug(uint32_t dv)
    {
        return dv == 96 ||
            dv == 97 ||
            dv == 98 ||
            dv == 99 ||
            dv == 100 ||
            dv == 101 ||
            dv == 102 ||
            dv == 103;
    }

    static bool IsRtRestirRayGenDebug(uint32_t dv)
    {
        return dv == 104 ||
            dv == 105;
    }

    static bool IsRtRestirTemporalDebug(uint32_t dv)
    {
        return dv == 106 ||
            dv == 107;
    }

    static bool IsRtRestirSpatialDebug(uint32_t dv)
    {
        return dv == 108 ||
            dv == 109;
    }

    static bool IsRtRestirResolveDebug(uint32_t dv)
    {
        return dv == 110 ||
            dv == 111 ||
            dv == 112 ||
            dv == 113 ||
            dv == 114;
    }

    static bool IsRtRestirDebug(uint32_t dv)
    {
        return IsRtRestirRayGenDebug(dv) ||
            IsRtRestirTemporalDebug(dv) ||
            IsRtRestirSpatialDebug(dv) ||
            IsRtRestirResolveDebug(dv);
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
    void UpdateRtGeometryTable(uint32_t frameIndex, ID3D12Resource* restirResolveReservoir = nullptr);

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
        DescriptorAllocator::Allocation& table,
        uint32_t& tableSrvCount,
        ID3D12Resource* surfaceIdResource);
    
    void CreateRtHistoryResources(ID3D12Device* device, uint32_t width, uint32_t height);

    bool UpdateRtSvgfSrvTable(
        uint32_t frameIndex,
        uint32_t iter,
        ID3D12Device* device,
        DescriptorAllocator::Allocation& table,
        uint32_t& tableSrvCount,
        ID3D12Resource* signalResource,
        ID3D12Resource* momentsResource,
        ID3D12Resource* motionConfResource,
        ID3D12Resource* viewZResource,
        ID3D12Resource* viewZConfResource,
        ID3D12Resource* surfaceIdResource);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtAtrousConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        uint32_t iterationIndex,
        bool useMoments,
        bool finalOutputSrgb,
        float motionConfPower,
        float motionConfMin,
        float viewZSigmaScale);

    void UpdateRtSvgfPingUavTable(ID3D12Device* device);

    bool UpdateRtTemporalTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* currAccumResource,
        ID3D12Resource* prevAccumResource,
        ID3D12Resource* prevMomentsResource,
        ID3D12Resource* outAccumResource,
        ID3D12Resource* outMomentsResource,
        ID3D12Resource* currViewZResource,
        ID3D12Resource* prevViewZResource,
        ID3D12Resource* prevViewZConfResource,
        ID3D12Resource* currSurfaceIdResource,
        ID3D12Resource* prevSurfaceIdResource,
        DescriptorAllocator::Allocation& srvTable,
        uint32_t& srvTableCount,
        DescriptorAllocator::Allocation& uavTable,
        uint32_t& uavTableCount);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtTemporalConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        float temporalAlpha,
        float roughnessSigma,
        float motionConfMin,
        float motionConfPower,
        float viewZSigmaScale,
        bool surfaceIdHistoryValid,
        float historyClampStrength,
        float signalDeltaSigma,
        float confidencePower,
        float antiLagStrength);

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
        const char* markerName,
        bool diffuseIsDemodulated);

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
        uint32_t& srvTableCount,
        ID3D12Resource* outputResource,
        D3D12_GPU_DESCRIPTOR_HANDLE outputUav,
        uint32_t width,
        uint32_t height,
        float motionConfMin,
        float motionConfPower,
        ID3D12Resource* surfaceIdResource);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtRayGenConstants(uint32_t frameIndex, uint32_t restirDispatchMode = 0);
    void CommitRtMotionWorlds();

    bool UpdateRtMotionDilateTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        bool useReconstructedViewZ);

    bool RunRtMotionDilate(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        bool useReconstructedViewZ);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtMotionDilateConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

    bool UpdateRtViewZReconstructTables(
        uint32_t frameIndex,
        ID3D12Device* device);

    bool RunRtViewZReconstruct(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtViewZReconstructConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

    void CommitRtViewZHistory(
        CommandList& cl,
        uint32_t writeIndex);

    bool UpdateRtDiffuseDemodulateTables(
        uint32_t frameIndex,
        ID3D12Device* device);

    bool RunRtDiffuseDemodulate(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtDiffuseDemodulateConstants(
        uint32_t frameIndex);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtCombineConstants(
        uint32_t frameIndex,
        bool diffuseIsDemodulated);

    RtDebugRouting BuildRtDebugRouting(uint32_t debugView) const;
    RtPostMode ResolveRtPostMode(const RtDebugRouting& debug) const;

    RtDenoiserGuides BuildRtDenoiserGuides() const;
    RtDenoiserHistories BuildRtDenoiserHistories(
        uint32_t readIndex,
        uint32_t writeIndex) const;

    DescriptorAllocator::Allocation& EnsureRtDescriptorTable(
        DescriptorAllocator::Allocation& table,
        uint32_t& currentCount,
        uint32_t expectedCount);

    void RunRtDenoiser(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height);

    void CommitRtSurfaceIdHistory(
        CommandList& cl,
        uint32_t writeIndex);

    bool UpdateRtOutlierClampTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        bool specSignal,
        ID3D12Resource* inputResource,
        ID3D12Resource* outputResource,
        bool useViewZ,
        bool useMotionConf,
        bool writeDebug);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtOutlierClampConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        bool specSignal,
        bool useViewZ,
        bool useMotionConf,
        bool writeDebug);

    bool RunRtOutlierClamp(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        bool specSignal,
        ID3D12Resource* inputResource,
        ID3D12Resource* outputResource,
        bool useViewZ,
        bool useMotionConf,
        bool writeDebug);

    bool LoadRtEnvironmentCpuRadiance(const std::filesystem::path& path);
    void ClearRtEnvironmentCpuRadiance();

    bool EnsureRtEnvironmentAlias(ID3D12Device* device, CommandList& cl);
    void WriteNullRtEnvAliasSrv(D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void WriteRtEnvAliasSrv(D3D12_CPU_DESCRIPTOR_HANDLE dst) const;

    void EnsureRtRestirResources(uint32_t width, uint32_t height);
    void ResetRtRestirResources();
    void ResetRtRestirHistory();

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtRestirTemporalConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height,
        bool temporalHistoryValid,
        bool surfaceIdHistoryValid,
        bool viewZHistoryValid);

    bool UpdateRtRestirTemporalTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* prevTemporalReservoir,
        ID3D12Resource* outTemporalReservoir,
        ID3D12Resource* currPrevUvResource,
        ID3D12Resource* currViewZResource,
        ID3D12Resource* prevViewZResource,
        ID3D12Resource* prevNormalResource,
        ID3D12Resource* prevDepthResource,
        ID3D12Resource* prevSurfaceIdResource,
        uint32_t width,
        uint32_t height);

    bool RunRtRestirTemporal(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        const RtDenoiserGuides& guides,
        bool ranViewZReconstruct,
        bool ranMotionDilate,
        const RtDebugRouting& rtDebug);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtRestirSpatialConstants(
        uint32_t frameIndex,
        uint32_t width,
        uint32_t height);

    bool UpdateRtRestirSpatialTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* temporalReservoir,
        ID3D12Resource* currNormalResource,
        ID3D12Resource* currDepthResource,
        ID3D12Resource* currSurfaceIdResource,
        ID3D12Resource* currViewZResource,
        ID3D12Resource* outSpatialReservoir,
        uint32_t width,
        uint32_t height);

    bool RunRtRestirSpatial(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        const RtDenoiserGuides& guides,
        bool ranViewZReconstruct,
        bool ranRestirTemporal,
        const RtDebugRouting& rtDebug);

    bool RunRtRestirResolve(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        float sceneTime,
        const RtDebugRouting& rtDebug);

    bool UpdateRtRestirApplyTables(
        uint32_t frameIndex,
        ID3D12Device* device,
        ID3D12Resource* baseDiffuse,
        ID3D12Resource* baseSpec,
        ID3D12Resource* restirDiffuse,
        ID3D12Resource* restirSpec,
        ID3D12Resource* outDiffuse,
        ID3D12Resource* outSpec);

    bool RunRtRestirApplyBeauty(
        CommandList& cl,
        uint32_t frameIndex,
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        const RtDebugRouting& rtDebug);

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtRestirApplyConstants(uint32_t frameIndex);

    TrianglePass m_triangle;
    UploadArena  m_upload;
    DXGI_FORMAT  m_backbufferFormat = DXGI_FORMAT_UNKNOWN;

    ForwardPBRPass m_forwardPbr;
    SceneResources m_scene;
    SceneData m_sceneData;
    float m_camYaw = 0.0f;
    float m_camPitch = -0.25f;
    float m_camRadius = 4.0f;
    CameraControlMode m_cameraMode = CameraControlMode::AutoOrbit;
    float m_lastCameraUpdateTime = 0.0f;
    bool m_cameraTimeValid = false;

    DirectX::XMFLOAT3 m_freeCamPosition{ 0.0f, 1.5f, 4.0f };
    float m_freeCamYaw = 0.0f;
    float m_freeCamPitch = 0.0f;
    bool m_freeCamInitialised = false;

    uint64_t m_cameraRevision = 0;
    uint64_t m_prevRtCameraRevision = 0;
    
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
    //   56 = motion confidence consumed by temporal
    //   57 = alpha after motion-confidence scaling
    //   58 = post-power motion confidence used for temporal weighting
    //   71 = temporal hit-distance weight visible on spec reuse
    //   72 = mismatch mask highlights rejected spec history
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
    //   73 = spec A-Trous hit-distance shaped weight
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
    // ViewZ / RayGen-owned views:
    // These write directly to m_rtOutput from RayGen.
    // The RT post stack must stay disabled.
    //   61 = raw ViewZ heatmap
    //   62 = invalid raw ViewZ mask
    // 
    // ViewZ reconstruction / compute-owned producer views:
    // These are produced by RtViewZReconstruct.
    // The RT post stack must stay disabled, but the RunRtViewZReconstruct pass still runs explicitly.
    //   63 = reconstructed ViewZ heatmap
    //   64 = reconstructed ViewZ confidence
    //   79 = normalized reconstructed ViewZ comparison-space value
    //   80 = invalid normalized ViewZ mask
    //
    // SurfaceId / RayGen-owned views:
    // These write directly to m_rtOutput from RayGen.
    // The RT post stack must stay disabled.
    //   65 = surfaceId visualization
    //   66 = invalid surfaceId mask
    //   74 = surfaceId visualization
    //   75 = invalid surfaceId mask
    // 
    // Diffuse albedo / RayGen-owned views:
    // These write directly to m_rtOutput from RayGen.
    // The RT post stack must stay disabled.
    //   67 = diffuse albedo visualization
    //   68 = invalid/near-zero diffuse albedo mask
    //
    // Diffuse demodulation / compute-owned producer views:
    // These are produced by RtDiffuseDemodulatePass.
    // The RT post stack must stay disabled, but the demodulate pass still runs explicitly.
    //   69 = demodulated diffuse lighting visualization
    //   70 = demodulation instability mask
    // 
    // Outlier clamp / compute-owned producer views:
    //   81 = diffuse outlier clamp factor
    //   82 = specular outlier clamp factor
    //   83 = invalid/NaN/Inf sanitized mask
    //   86 = outlier neighborhood valid-weight heatmap
    //
    // Temporal robustness / temporal-owned views:
    //   84 = temporal history color clamp amount
    //   85 = moment variance clamp mask
    // 
    // Temporal shaping / temporal-owned views:
    //   87 = temporal signal confidence
    //   88 = temporal anti-lag responsiveness amount
    //   89 = confidence-shaped history length
    //   90 = temporal luminance delta confidence
    //
    // Spec history shaping / history-select-owned views:
    //   91 = spec responsive selection weight
    //   92 = spec stable history confidence
    //
    // A-Trous shaping / A-Trous-owned views:
    //   93 = adaptive blur strength
    //   94 = wide-iteration suppression mask; black when spec A-Trous (m_rtAtrousIterationsSpec + prev) runs fewer than 3 iterations
    //   95 = variance/history instability
    // 
    // RT environment sampling / RayGen-owned views:
    // These are RT sampling / environment importance-sampling inspection views.
    // The RT post stack must stay disabled.
    //   96  = env alias/PDF heatmap; horizontal pixel coordinate scans the alias table
    //   97  = sampled env direction/PDF visualization
    //   98  = env NEE MIS weight heatmap
    //   99  = env visibility mask
    //   100 = direct env NEE luminance
    //   101 = BRDF env-hit luminance approximation after env-hit MIS
    //   102 = env sampling technique/mode; red = BRDF-only, green = env-only, blue = MIS reference
    //   103 = env sampling fallback/readiness mask; magenta = invalid/fallback, green = ready
    // 
    // ReSTIR Env DI / Phase 10.7 views:
    // These inspect the ReSTIR environment direct-light reservoir pipeline.
    // ReSTIR is environment-direct only here: no RTXDI, no GI, no many-light path.
    //
    // Initial reservoir / RayGen-owned views:
    // These are produced during the primary DXR dispatch. ClosestHit writes
    // g_RestirInitialReservoir, then RayGen visualizes it.
    // The RT post stack must stay disabled.
    //   104 = initial ReSTIR reservoir target luminance heatmap
    //   105 = initial ReSTIR reservoir selected source PDF heatmap
    //
    // Temporal reservoir / compute-owned views:
    // These are produced by RtRestirTemporalPass and write directly to m_rtOutput.
    // The RT post stack must stay disabled, but ReSTIR temporal history must still advance.
    //   106 = temporal previous-frame reservoir reuse accepted mask; white = previous reused
    //   107 = temporal reservoir M / confidence; RG = M heat, B = confidence
    //
    // Spatial reservoir / compute-owned views:
    // These are produced by RtRestirSpatialPass and write directly to m_rtOutput.
    // The RT post stack must stay disabled.
    //   108 = spatial accepted neighbor reuse count heatmap
    //   109 = selected spatial neighbor distance heatmap
    //
    // ReSTIR resolve / DXR-owned views:
    // These are produced by the ReSTIR DXR resolve dispatch.
    // Resolve consumes the selected reservoir, traces visibility, and writes
    // g_RestirResolvedDiffuse / g_RestirResolvedSpec.
    // Initial reservoir generation must not run during resolve dispatch.
    //   110 = resolved reservoir final W heatmap
    //   111 = resolved visibility mask; white = selected env direction visible
    //   112 = resolved diffuse luminance heatmap
    //   113 = resolved specular luminance heatmap
    //   114 = resolve invalid reason mask; R = invalid reservoir, G = SurfaceId mismatch, B = geometric reject / visibility reject / primary miss
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
    bool m_pauseAnimation = false;
    bool m_useRaytracing = false;        // Toggle for raytracing vs rasterization (for testing/debugging)
    bool m_rtAccumulate = false;         // validation / progressive mode
    bool m_rtSvgf = true;
    bool  m_rtDenoise = true;

    bool m_rtEnableRestirEnvDi = false;
    bool m_rtRestirUseTemporal = true;
    bool m_rtRestirUseSpatial = true;
    bool m_rtRestirResolveToBeauty = false;

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

    // RT environment importance sampling.
    // CPU builder owns the alias/PDF data; GPU resources are uploaded later.
    RtEnvironmentImportance m_rtEnvImportance;

    ComPtr<ID3D12Resource> m_rtEnvAliasBuffer;
    ComPtr<ID3D12Resource> m_rtEnvAliasUpload;

    bool m_rtEnvAliasReady = false;
    bool m_rtEnvAliasDirty = true;

    uint32_t m_rtEnvAliasCount = 0;
    uint32_t m_rtEnvFaceSize = 0;
    float m_rtEnvTotalWeight = 0.0f;
    bool m_rtEnvAliasFallback = true;

    // Start conservative for validation. After debug views/energy parity pass,
    // this can move to MisOneSample.
    RtEnvSamplingMode m_rtEnvSamplingMode = RtEnvSamplingMode::BrdfOnly;

    bool m_rtUseEnvImportanceSampling = true;
    bool m_rtUseEnvMIS = false;
    float m_rtEnvMISPower = 2.0f;
    float m_rtEnvIntensity = 1.0f;
    float m_rtEnvPdfEpsilon = 1e-6f;
    float m_rtEnvDeltaRoughnessCutoff = 0.04f;
    bool m_rtUseEnvNeeForFinal = false;
    bool m_prevRtUseEnvNeeForFinal = false;

    bool  m_prevRtEnableIndirect = true;
    float m_prevRtIndirectScale = 1.0f;

    // Previous-frame copies for accumulation/history reset detection.
    RtEnvSamplingMode m_prevRtEnvSamplingMode = RtEnvSamplingMode::MisTwoSampleReference;
    bool m_prevRtUseEnvImportanceSampling = true;
    bool m_prevRtUseEnvMIS = true;
    float m_prevRtEnvMISPower = 2.0f;
    float m_prevRtEnvIntensity = 1.0f;
    float m_prevRtEnvPdfEpsilon = 1e-6f;
    float m_prevRtEnvDeltaRoughnessCutoff = 0.04f;

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

        // per-frame RT table: geometry + instance data + material textures + RT IBL/sampling SRVs.
        DescriptorAllocator::Allocation geometryTable{};
        uint32_t geometryTableCount = 0;

        DescriptorAllocator::Allocation temporalDiffuseSrvTable{};     // kRtTemporalSrvCount SRVs
        DescriptorAllocator::Allocation temporalDiffuseUavTable{};     // kRtTemporalUavCount UAVs
        uint32_t temporalDiffuseSrvCount = 0;
        uint32_t temporalDiffuseUavCount = 0;

        DescriptorAllocator::Allocation temporalSpecStableSrvTable{};  // kRtTemporalSrvCount SRVs
        DescriptorAllocator::Allocation temporalSpecStableUavTable{};  // kRtTemporalUavCount UAVs
        uint32_t temporalSpecStableSrvCount = 0;
        uint32_t temporalSpecStableUavCount = 0;

        DescriptorAllocator::Allocation temporalSpecRespSrvTable{};    // kRtTemporalSrvCount SRVs
        DescriptorAllocator::Allocation temporalSpecRespUavTable{};    // kRtTemporalUavCount UAVs
        uint32_t temporalSpecRespSrvCount = 0;
        uint32_t temporalSpecRespUavCount = 0;

        DescriptorAllocator::Allocation historySelectSrvTable{};  // kRtHistorySelectSrvCount SRVs
        DescriptorAllocator::Allocation historySelectUavTable{};  // kRtHistorySelectUavCount UAVs
        uint32_t historySelectSrvCount = 0;
        uint32_t historySelectUavCount = 0;
        DescriptorAllocator::Allocation denoiseDiffuseSrvTable{}; // kRtDenoiseSrvCount SRVs
        DescriptorAllocator::Allocation denoiseSpecSrvTable{};    // kRtDenoiseSrvCount SRVs
        DescriptorAllocator::Allocation combineSrvTable{};        // kRtCombineSrvCount SRVs
        uint32_t denoiseDiffuseSrvCount = 0;
        uint32_t denoiseSpecSrvCount = 0;
        uint32_t combineSrvCount = 0;

        // Per-frame, per-iteration SVGF input tables.
        std::array<DescriptorAllocator::Allocation, kMaxRtAtrousIterations> svgfSpecSrvTables{};
        std::array<DescriptorAllocator::Allocation, kMaxRtAtrousIterations> svgfDiffuseSrvTables{};

        std::array<uint32_t, kMaxRtAtrousIterations> svgfSpecSrvCounts{};
        std::array<uint32_t, kMaxRtAtrousIterations> svgfDiffuseSrvCounts{};

        DescriptorAllocator::Allocation motionDilateSrvTable{}; // kRtMotionDilateSrvCount SRVs
        DescriptorAllocator::Allocation motionDilateUavTable{}; // kRtMotionDilateUavCount UAVs
        uint32_t motionDilateSrvCount = 0;
        uint32_t motionDilateUavCount = 0;

        uint32_t capacity = 0;

        DescriptorAllocator::Allocation viewZReconstructSrvTable{}; // kRtViewZReconstructSrvCount SRVs
        DescriptorAllocator::Allocation viewZReconstructUavTable{}; // kRtViewZReconstructUavCount UAVs
        uint32_t viewZReconstructSrvCount = 0;
        uint32_t viewZReconstructUavCount = 0;

        DescriptorAllocator::Allocation diffuseDemodSrvTable{}; // kRtDiffuseDemodSrvCount SRVs
        DescriptorAllocator::Allocation diffuseDemodUavTable{}; // kRtDiffuseDemodUavCount UAVs
        uint32_t diffuseDemodSrvCount = 0;
        uint32_t diffuseDemodUavCount = 0;

        DescriptorAllocator::Allocation outlierDiffuseSrvTable{}; // kRtOutlierClampSrvCount SRVs
        DescriptorAllocator::Allocation outlierDiffuseUavTable{}; // kRtOutlierClampUavCount UAVs
        uint32_t outlierDiffuseSrvCount = 0;
        uint32_t outlierDiffuseUavCount = 0;

        DescriptorAllocator::Allocation outlierSpecSrvTable{}; // kRtOutlierClampSrvCount SRVs
        DescriptorAllocator::Allocation outlierSpecUavTable{}; // kRtOutlierClampUavCount UAVs
        uint32_t outlierSpecSrvCount = 0;
        uint32_t outlierSpecUavCount = 0;

        DescriptorAllocator::Allocation restirTemporalSrvTable{};
        DescriptorAllocator::Allocation restirTemporalUavTable{};
        uint32_t restirTemporalSrvCount = 0;
        uint32_t restirTemporalUavCount = 0;

        DescriptorAllocator::Allocation restirSpatialSrvTable{};
        DescriptorAllocator::Allocation restirSpatialUavTable{};
        uint32_t restirSpatialSrvCount = 0;
        uint32_t restirSpatialUavCount = 0;

        DescriptorAllocator::Allocation restirApplySrvTable{};
        DescriptorAllocator::Allocation restirApplyUavTable{};
        uint32_t restirApplySrvCount = 0;
        uint32_t restirApplyUavCount = 0;
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
    // u6 = m_rtAovViewZRaw  R16_FLOAT visible-surface RayT, -1 invalid
    // u7 = m_rtAovSurfaceId    R32_UINT object/material id, 0xFFFFFFFF invalid
    // u8 = m_rtAovDiffuseAlbedo R16G16B16A16_FLOAT rgb=diffuse albedo, a=stable demod flag
    // u9  = m_rtRestirInitialReservoir StructuredBuffer/RWStructuredBuffer<RtRestirReservoir>
    // u10 = m_rtRestirResolvedDiffuse  R16G16B16A16_FLOAT
    // u11 = m_rtRestirResolvedSpec     R16G16B16A16_FLOAT
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
    static constexpr uint32_t kRtSamplingSrvCount = 1;
    static constexpr uint32_t kRtRestirResolveSrvCount = 1;

    static constexpr uint32_t kRtSrvTableCountWithoutSampling =
        kRtGeometrySrvCount +
        (kRtTexturesPerMaterial * kMaxRtMaterials) +
        kRtIblSrvCount;

    static constexpr uint32_t kRtSrvTableCount =
        kRtSrvTableCountWithoutSampling +
        kRtSamplingSrvCount +
        kRtRestirResolveSrvCount;

    static constexpr uint32_t kRtSrv_BrdfLut =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 0; // slot 29, HLSL t30

    static constexpr uint32_t kRtSrv_IblDiff =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 1; // slot 30, HLSL t31

    static constexpr uint32_t kRtSrv_IblSpec =
        kRtGeometrySrvCount + kRtTexturesPerMaterial * kMaxRtMaterials + 2; // slot 31, HLSL t32

    static constexpr uint32_t kRtSrv_EnvAlias =
        kRtSrvTableCountWithoutSampling; // slot 32, HLSL t33

    static constexpr uint32_t kRtSrv_RestirResolve =
        kRtSrv_EnvAlias + 1; // slot 33, HLSL t34

    static constexpr uint32_t kRtEnvImportanceFaceSize = 128;
    static constexpr uint32_t kRtEnvImportanceFallbackFaceSize = 64;


    ComPtr<ID3D12Resource> m_rtAovNormal;
    ComPtr<ID3D12Resource> m_rtAovDepth;

    ComPtr<ID3D12Resource> m_rtAovMotion;
    bool m_rtAovMotionReady = false;

    ComPtr<ID3D12Resource> m_rtAovMotionDilated;
    bool m_rtAovMotionDilatedReady = false;
    ComPtr<ID3D12Resource> m_rtAovMotionConf;
    bool m_rtAovMotionConfReady = false;
    ComPtr<ID3D12Resource> m_rtAovViewZRaw;
    bool m_rtAovViewZRawReady = false;

    ComPtr<ID3D12Resource> m_rtAovSurfaceId;
    bool m_rtAovSurfaceIdReady = false;

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

    float m_rtViewZSigmaScale = 0.5f;
    float m_prevRtViewZSigmaScale = 0.5f;

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

    static constexpr uint32_t kRtUavTableCount = 12;
    static constexpr uint32_t kRtHistorySelectSrvCount = 7;
    static constexpr uint32_t kRtHistorySelectUavCount = 3;
    static constexpr uint32_t kRtSvgfSrvCount = 8;
    static constexpr uint32_t kRtDenoiseSrvCount = 5;
    static constexpr uint32_t kRtMotionDilateSrvCount = 5;
    static constexpr uint32_t kRtMotionDilateUavCount = 3;
    static constexpr uint32_t kRtViewZReconstructSrvCount = 8;
    static constexpr uint32_t kRtViewZReconstructUavCount = 3;
    static constexpr uint32_t kRtDiffuseDemodSrvCount = 3;
    static constexpr uint32_t kRtDiffuseDemodUavCount = 2;
    static constexpr uint32_t kRtCombineSrvCount = 3;
    static constexpr uint32_t kRtTemporalSrvCount = 14;
    static constexpr uint32_t kRtTemporalUavCount = 3;
    static constexpr float kRtViewZRoughCutoff = 0.35f;
    static constexpr float kRtViewZConfMin = 0.5f;
    static constexpr float kRtDistanceNormSigma = 0.08f;
    static constexpr uint32_t kRtOutlierClampSrvCount = 7;
    static constexpr uint32_t kRtOutlierClampUavCount = 2;
    static constexpr bool kRtEnvNeeFireflyGuardDefault = true;
    static constexpr uint32_t kRtRestirTemporalSrvCount = 11;
    static constexpr uint32_t kRtRestirTemporalUavCount = 2;
    static constexpr uint32_t kRtRestirSpatialSrvCount = 5;
    static constexpr uint32_t kRtRestirSpatialUavCount = 2;
    static constexpr uint32_t kRtRestirApplySrvCount = 4;
    static constexpr uint32_t kRtRestirApplyUavCount = 2;

    RtDiffuseDemodulatePass m_rtDiffuseDemodulatePass;


    RtMotionDilatePass m_rtMotionDilatePass;
    static constexpr uint32_t kMaxRtMotionDilateRadius = 4;
    uint32_t m_rtMotionDilateRadius = 2;
    float m_rtMotionDilateDepthSigma = 0.02f;
    float m_rtMotionDilateNormalSigma = 0.25f;
    float m_rtMotionDilateMinScore = 0.05f;

    RtHitDistReconstructPass m_rtViewZReconstructPass;

    ComPtr<ID3D12Resource> m_rtAovViewZRecons;
    ComPtr<ID3D12Resource> m_rtAovViewZReconsConf;
    bool m_rtAovViewZReconsReady = false;
    bool m_rtAovViewZReconsConfReady = false;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryViewZ{};
    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistoryViewZConf{};
    bool m_rtViewZHistoryValid = false;

    float m_rtViewZReconsAlpha = 0.20f;
    float m_prevRtViewZReconsAlpha = 0.20f;

    ComPtr<ID3D12Resource> m_rtAovDiffuseAlbedo;
    bool m_rtAovDiffuseAlbedoReady = false;

    ComPtr<ID3D12Resource> m_rtDiffuseDemodulated;
    bool m_rtDiffuseDemodulatedReady = false;

    std::array<ComPtr<ID3D12Resource>, 2> m_rtHistorySurfaceId{};
    bool m_rtSurfaceIdHistoryValid = false;

    RtOutlierClampPass m_rtOutlierClampPass;

    ComPtr<ID3D12Resource> m_rtDiffuseRobustInput;
    bool m_rtDiffuseRobustInputReady = false;

    ComPtr<ID3D12Resource> m_rtSpecRobustInput;
    bool m_rtSpecRobustInputReady = false;

    bool m_rtEnableOutlierClamp = true;
    bool m_rtEnableRobustMoments = true;

    float m_rtOutlierClampMaxLuminance = 250.0f;
    float m_rtOutlierClampDiffuseSigmaK = 4.0f;
    float m_rtOutlierClampSpecSigmaK = 6.0f;

    float m_rtTemporalHistoryClampStrengthDiffuse = 0.5f;
    float m_rtTemporalHistoryClampStrengthSpec = 0.75f;
    float m_rtTemporalNeighborhoodSigmaK = 4.0f;
    float m_rtTemporalClampMinWeight = 1.0f;
    float m_rtTemporalClampRelaxation = 0.5f;

    float m_rtAtrousContributionMaxLuminance = 250.0f;

    float m_rtOutlierClampDiffuseStrength = 1.0f;
    float m_rtOutlierClampSpecStrength = 0.5f;

    bool m_rtEnableSignalConfidence = true;

    float m_rtSignalDeltaSigmaDiffuse = 1.5f;
    float m_rtSignalDeltaSigmaSpec = 2.5f;

    float m_rtConfidencePowerDiffuse = 1.0f;
    float m_rtConfidencePowerSpec = 1.5f;

    float m_rtMinSignalConfidence = 0.05f;

    float m_rtAntiLagStrengthDiffuse = 0.35f;
    float m_rtAntiLagStrengthSpec = 0.55f;

    float m_rtVarianceConfidenceScale = 2.0f;
    float m_rtHistoryLengthConfidencePower = 0.75f;
    float m_rtResponsiveAlphaBoost = 2.0f;

    float m_rtMaxStableHistory = 255.0f;
    float m_rtMinStableHistoryForClamp = 2.0f;
    float m_rtConfidenceDebugScale = 1.0f;

    bool m_rtEnableSpecHistoryShaping = true;

    float m_rtSpecResponsiveRoughnessCutoff = 0.35f;
    float m_rtSpecResponsiveVarianceScale = 2.0f;
    float m_rtSpecResponsiveMotionScale = 1.0f;

    float m_rtSpecStableMinHistory = 8.0f;
    float m_rtSpecResponsiveBias = 0.0f;
    float m_rtSpecHistoryBlendPower = 1.0f;

    bool m_rtEnableAdaptiveAtrous = true;

    float m_rtAdaptiveVarianceScaleDiffuse = 1.0f;
    float m_rtAdaptiveVarianceScaleSpec = 1.5f;

    float m_rtAdaptiveHistoryMin = 1.0f;
    float m_rtAdaptiveHistoryMax = 16.0f;

    float m_rtDiffuseBlurBoost = 1.0f;
    float m_rtSpecBlurRoughnessBoost = 0.75f;
    float m_rtSpecGlossyBlurLimit = 0.35f;
    float m_rtWideIterationConfidenceMin = 0.35f;

    float m_rtAdaptiveSigmaLMin = 0.5f;
    float m_rtAdaptiveSigmaLMax = 2.0f;
    float m_rtAdaptiveNormalRelaxation = 0.25f;

    std::vector<DirectX::XMFLOAT3> m_rtEnvCpuRadiance;
    uint32_t m_rtEnvCpuFaceSize = 0;
    bool m_rtEnvCpuRadianceReady = false;

    bool m_rtEnvNeeFireflyGuard = kRtEnvNeeFireflyGuardDefault;
    float m_rtEnvNeeMaxRadiance = 20.0f;

    bool m_prevRtEnvNeeFireflyGuard = kRtEnvNeeFireflyGuardDefault;
    float m_prevRtEnvNeeMaxRadiance = 20.0f;

    uint32_t m_rtEnvAliasVersion = 0;
    uint32_t m_prevRtEnvAliasVersion = 0;

    // -------------------------------------------------------------------------
    // ReSTIR environment direct-light resources/state
    // -------------------------------------------------------------------------
    // Reservoir pipeline:
    //   DXR primary pass -> initial environment-light reservoir
    //   compute          -> temporal reservoir reuse
    //   compute          -> spatial reservoir reuse
    //   DXR resolve      -> visibility-tested diffuse/specular contribution
    //   compute/apply    -> optional validation apply into the RT signal buffers
    //
    // This path samples environment direct lighting only. It does not own GI,
    // many-light sampling, material-light sampling, or recursive reuse.
    //
    // m_rtRestirHistoryValid tracks reservoir history only. It is intentionally
    // separate from raw RT accumulation history and SVGF signal history.
    ComPtr<ID3D12Resource> m_rtRestirInitialReservoir;
    ComPtr<ID3D12Resource> m_rtRestirTemporalReservoir[2];
    ComPtr<ID3D12Resource> m_rtRestirSpatialReservoir;

    ComPtr<ID3D12Resource> m_rtRestirResolvedDiffuse;
    ComPtr<ID3D12Resource> m_rtRestirResolvedSpec;
    ComPtr<ID3D12Resource> m_rtRestirAppliedDiffuse;
    ComPtr<ID3D12Resource> m_rtRestirAppliedSpec;

    bool m_rtRestirResourcesReady = false;
    bool m_rtRestirHistoryValid = false;

    bool m_rtRestirTemporalValidThisFrame = false;
    bool m_rtRestirSpatialValidThisFrame = false;
    bool m_rtRestirResolvedValidThisFrame = false;
    bool m_rtRestirAppliedReady = false;

    uint32_t m_rtRestirHistoryReadIndex = 0;
    uint32_t m_rtRestirHistoryWriteIndex = 1;


    // Validation-only post-denoise ReSTIR apply scale.
    // This is not a physical ReSTIR weight; production integration should apply
    // the resolved direct term before temporal/A-trous filtering.
    float m_rtRestirApplyDiffuseScale = 0.25f;
    float m_rtRestirApplySpecularScale = 0.25f;
    uint32_t m_rtRestirApplyMode = 1; // validation additive

    // ReSTIR validation defaults.
    // CandidateCount = 16 gives denser initial reservoirs for inspection at
    // extra cost. ReSTIR is opt-in, so this does not affect default rendering.
    //
    // Temporal reuse is intentionally conservative. Longer reservoir age can
    // ghost on animated or rotating geometry unless reuse confidence is high.
    uint32_t m_rtRestirInitialCandidateCount = 16;
    uint32_t m_rtRestirSpatialSamples = 4;
    uint32_t m_rtRestirSpatialRadius = 8;

    float m_rtRestirNormalSigma = 0.1f;
    float m_rtRestirDepthSigma = 0.02f;
    float m_rtRestirViewZSigma = 0.25f; 
    float m_rtRestirRoughnessSigma = 0.20f;

    float m_rtRestirMaxM = 32.0f;
    float m_rtRestirMaxAge = 2.0f; 
    float m_rtRestirMinTarget = 1e-5f;
    float m_rtRestirMaxWeight = 64.0f;
    float m_rtRestirTemporalMinConfidence = 0.65f; 

    //These let RenderFrame() detect when ReSTIR settings changed and clear reservoir history without unnecessarily destroying resources
    bool m_prevRtEnableRestirEnvDi = false;
    bool m_prevRtRestirUseTemporal = true;
    bool m_prevRtRestirUseSpatial = true;

    uint32_t m_prevRtRestirInitialCandidateCount = 16;
    uint32_t m_prevRtRestirSpatialSamples = 4;
    uint32_t m_prevRtRestirSpatialRadius = 8;

    float m_prevRtRestirNormalSigma = 0.1f;
    float m_prevRtRestirDepthSigma = 0.02f;
    float m_prevRtRestirViewZSigma = 0.25f;
    float m_prevRtRestirRoughnessSigma = 0.20f;

    float m_prevRtRestirMaxM = 32.0f;
    float m_prevRtRestirMaxAge = 2.0f;
    float m_prevRtRestirMinTarget = 1e-5f;
    float m_prevRtRestirMaxWeight = 64.0f;
    float m_prevRtRestirTemporalMinConfidence = 0.65f;

    RtRestirTemporalPass m_rtRestirTemporalPass;
    RtRestirSpatialPass m_rtRestirSpatialPass;
    RtRestirApplyPass m_rtRestirApplyPass; //validation additive apply, not final production integration

    D3D12_GPU_VIRTUAL_ADDRESS UpdateRtHistorySelectConstants(uint32_t frameIndex);
};

 
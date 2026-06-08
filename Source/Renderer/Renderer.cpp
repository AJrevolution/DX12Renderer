#include "Source/Renderer/Renderer.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/FrameConstants.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include "DrawConstants.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"
#include <algorithm>
#include <cstring>
#include <cmath>

namespace
{
    bool MatrixNear(const DirectX::XMFLOAT4X4& a, const DirectX::XMFLOAT4X4& b, float eps = 1e-4f)
    {
        const float* pa = reinterpret_cast<const float*>(&a);
        const float* pb = reinterpret_cast<const float*>(&b);

        for (int i = 0; i < 16; ++i)
        {
            if (std::fabs(pa[i] - pb[i]) > eps)
                return false;
        }

        return true;
    }

    static bool IsTemporalDebug(uint32_t dv)
    {
        return
            (dv >= 18 && dv <= 26) ||
            (dv >= 32 && dv <= 36) ||
            (dv >= 45 && dv <= 47) ||
            dv == 56 ||
            dv == 57 ||
            dv == 58 ||
            dv == 71 ||
            dv == 72 ||
            dv == 76 ||
            dv == 77 ||
            dv == 84 ||
            dv == 85 ||
            dv == 87 ||
            dv == 88 ||
            dv == 89 ||
            dv == 90;
    }

    static bool IsSvgfDebug(uint32_t dv)
    {
        return (dv == 28) ||
            (dv == 43) ||
            (dv == 44) ||
            dv == 60 ||
            dv == 73 ||
            dv == 78 ||
            dv == 93 ||
            dv == 94 ||
            dv == 95;
    }

    static bool IsHistorySelectDebug(uint32_t dv)
    {
        return
            (dv >= 29 && dv <= 31) ||
            (dv >= 37 && dv <= 42) ||
            dv == 59 ||
            dv == 91 ||
            dv == 92;
    }
}

void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;

    m_upload.Initialize(device, frameCount, 64 * 1024 * 1024);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
    
    // Shader-visible SRV heap for textures
    m_srvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true, L"SRV Heap (Shader Visible)");

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

bool Renderer::RtPostModeRunsTemporal(RtPostMode mode)
{
    switch (mode)
    {
    case RtPostMode::TemporalOnly:
    case RtPostMode::HistorySelectOnly:
    case RtPostMode::SpecAtrousOnly:
    case RtPostMode::DiffuseAtrousOnly:
    case RtPostMode::Full:
        return true;

    default:
        return false;
    }
}

bool Renderer::RtPostModeRunsHistorySelect(RtPostMode mode)
{
    switch (mode)
    {
    case RtPostMode::HistorySelectOnly:
    case RtPostMode::SpecAtrousOnly:
    case RtPostMode::DiffuseAtrousOnly:
    case RtPostMode::Full:
        return true;

    default:
        return false;
    }
}

bool Renderer::RtPostModeRunsSpecAtrous(RtPostMode mode)
{
    return mode == RtPostMode::SpecAtrousOnly ||
        mode == RtPostMode::Full;
}

bool Renderer::RtPostModeRunsDiffuseAtrous(RtPostMode mode)
{
    return mode == RtPostMode::DiffuseAtrousOnly ||
        mode == RtPostMode::Full;
}

bool Renderer::RtPostModeRunsAnyAtrous(RtPostMode mode)
{
    return RtPostModeRunsSpecAtrous(mode) ||
        RtPostModeRunsDiffuseAtrous(mode);
}

bool Renderer::RtPostModeCommitsHistory(RtPostMode mode)
{
    switch (mode)
    {
    case RtPostMode::TemporalOnly:
    case RtPostMode::HistorySelectOnly:
    case RtPostMode::SpecAtrousOnly:
    case RtPostMode::DiffuseAtrousOnly:
    case RtPostMode::Full:
        return true;

    default:
        return false;
    }
}

bool Renderer::CombineRtSignalsToOutput(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Resource* diffuseResource,
    ID3D12Resource* specularResource,
    uint32_t width,
    uint32_t height,
    const char* markerName,
    bool diffuseIsDemodulated)
{
    if (!m_rtOutputReady ||
        !m_rtAovDiffuseAlbedoReady ||
        diffuseResource == nullptr ||
        specularResource == nullptr)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, markerName);

    cl.Transition(diffuseResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(specularResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDiffuseAlbedo.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.FlushBarriers();

    const bool okCombineTable =
        UpdateRtCombineSrvTable(
            frameIndex,
            device,
            diffuseResource,
            specularResource);

    if (!okCombineTable)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtCombineConstants(
            frameIndex,
            diffuseIsDemodulated);

    m_rtCombinePass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].combineSrvTable.gpu,
        m_rtOutputUav.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = m_rtOutput.Get();
    cmdList->ResourceBarrier(1, &b);

    CmdEndEvent(cmdList);
    return true;
}

bool Renderer::RunRtRawCombineOnly(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    if (!m_rtAccumDiffuseReady || !m_rtAccumSpecReady)
        return false;

    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        m_rtAccumDiffuse.Get(),
        m_rtAccumSpec.Get(),
        width,
        height,
        "RT Raw Combine Only",
        false);
}

bool Renderer::RunRtTemporalOnlyCombine(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Resource* diffuseSignal,
    ID3D12Resource* specStableSignal,
    uint32_t width,
    uint32_t height)
{
    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        diffuseSignal,
        specStableSignal,
        width,
        height,
        "RT Temporal Only Combine",
        true);
}

bool Renderer::RunRtHistorySelectOnlyCombine(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Resource* diffuseSignal,
    ID3D12Resource* specSelectedSignal,
    uint32_t width,
    uint32_t height)
{
    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        diffuseSignal,
        specSelectedSignal,
        width,
        height,
        "RT History Select Only Combine",
        true);
}

bool Renderer::RunRtSpecAtrousOnlyCombine(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Resource* diffuseSignal,
    uint32_t width,
    uint32_t height)
{
    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        diffuseSignal,
        m_rtPostSpec.Get(),
        width,
        height,
        "RT Spec A-Trous Only Combine",
        true);
}

bool Renderer::RunRtDiffuseAtrousOnlyCombine(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Resource* specStableSignal,
    uint32_t width,
    uint32_t height)
{
    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        m_rtPostDiffuse.Get(),
        specStableSignal,
        width,
        height,
        "RT Diffuse A-Trous Only Combine",
        true);
}

bool Renderer::RunRtFinalCombine(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        m_rtPostDiffuse.Get(),
        m_rtPostSpec.Get(),
        width,
        height,
        "RT Combine",
        true);
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

    if (m_pauseAnimation && !m_wasPaused)
    {
        m_frozenTime = time;
    }

    const float sceneTime = m_pauseAnimation ? m_frozenTime : time;
    m_wasPaused = m_pauseAnimation;

    BuildDrawList(sceneTime);

    const RtDebugRouting rtDebug =
        BuildRtDebugRouting(m_debugView);

    const bool wantsSplitDebug = rtDebug.wantsSplitDebug;
    const bool wantsMotionDebug = rtDebug.wantsMotionDebug;
    const bool wantsMotionDilateDebug = rtDebug.wantsMotionDilateDebug;
    const bool wantsViewZDebug = rtDebug.wantsViewZDebug;
    const bool wantsViewZReconstructDebug = rtDebug.wantsViewZReconstructDebug;
    const bool wantsSurfaceIdDebug = rtDebug.wantsSurfaceIdDebug;
    const bool wantsDiffuseAlbedoDebug = rtDebug.wantsDiffuseAlbedoDebug;
    const bool wantsDiffuseDemodDebug = rtDebug.wantsDiffuseDemodDebug;
    const bool wantsRtInspectionDebug = rtDebug.wantsRtInspectionDebug;
    const bool wantsOutlierClampDebug = rtDebug.wantsOutlierClampDebug;

    RtPostMode rtPostMode = ResolveRtPostMode(rtDebug);

    const bool allowRtAccumulation =
        m_rtAccumulate &&
        !wantsMotionDilateDebug &&
        !wantsViewZReconstructDebug &&
        !wantsDiffuseDemodDebug &&
        !wantsOutlierClampDebug &&
        ((m_debugView == 0) || wantsRtInspectionDebug) &&
        (
            m_rtTemporal ||
            (!m_autoOrbit && m_pauseAnimation) ||
            wantsSplitDebug ||
            wantsMotionDebug ||
            wantsViewZDebug ||
            wantsSurfaceIdDebug ||
            wantsDiffuseAlbedoDebug
        );

    bool drawListStructuralChanged =
        !m_rtHistoryValid ||
        (m_draws.size() != m_prevRtWorlds.size()) ||
        (m_draws.size() != m_prevRtMaterials.size());

    bool drawTransformChanged = !m_rtHistoryValid;

    if (!drawListStructuralChanged)
    {
        for (size_t i = 0; i < m_draws.size(); ++i)
        {
            if (m_prevRtMaterials[i] != m_draws[i].material ||
                m_draws[i].mesh == nullptr)
            {
                drawListStructuralChanged = true;
                break;
            }

            if (!MatrixNear(m_prevRtWorlds[i], m_draws[i].world))
            {
                drawTransformChanged = true;
            }
        }
    }

    const bool drawListChanged =
        drawListStructuralChanged || drawTransformChanged;

    if (drawListStructuralChanged)
    {
        m_prevRtMotionWorldsValid = false;
        m_rtSurfaceIdHistoryValid = false;
    }

    const bool svgfChanged =
        !m_rtHistoryValid ||
        (m_rtSvgf != m_prevRtSvgf) ||
        (m_rtAtrousIterations != m_prevRtAtrousIterations) ||
        (m_rtAtrousIterationsSpec != m_prevRtAtrousIterationsSpec) ||
        (std::fabs(m_rtVarianceScale - m_prevRtVarianceScale) > 1e-6f) ||
        (std::fabs(m_rtAtrousLengthAttenuation - m_prevRtAtrousLengthAttenuation) > 1e-6f) ||
        (std::fabs(m_rtAtrousLengthPower - m_prevRtAtrousLengthPower) > 1e-6f) ||
        (std::fabs(m_rtAtrousLengthSkipThreshold - m_prevRtAtrousLengthSkipThreshold) > 1e-6f) ||
        (m_rtAtrousEnableLengthSkip != m_prevRtAtrousEnableLengthSkip);

    
    const bool bigCameraChanged =
        !m_rtHistoryValid ||
        (std::fabs(m_camYaw - m_prevRtCamYaw) > 0.05f) ||
        (std::fabs(m_camPitch - m_prevRtCamPitch) > 0.05f) ||
        (std::fabs(m_camRadius - m_prevRtCamRadius) > 0.10f);

    const bool cameraChanged =
        !m_rtHistoryValid ||
        (std::fabs(m_camYaw - m_prevRtCamYaw) > 1e-6f) ||
        (std::fabs(m_camPitch - m_prevRtCamPitch) > 1e-6f) ||
        (std::fabs(m_camRadius - m_prevRtCamRadius) > 1e-6f);

    const bool debugViewChanged =
        !m_rtHistoryValid ||
        (m_debugView != m_prevRtDebugView);

    const bool accumulationModeChanged =
        (allowRtAccumulation != m_rtAccumulatingLastFrame);

    const bool integratorChanged =
        !m_rtHistoryValid ||
        (m_rtEnableIndirect != m_prevRtEnableIndirect) ||
        (std::fabs(m_rtIndirectScale - m_prevRtIndirectScale) > 1e-6f);

    const bool rtHasBrdfLut = m_brdfLutTex.IsValid();
    const bool rtHasIbl = m_iblDiffuseTex.IsValid() && m_iblSpecularTex.IsValid();

    const bool iblAvailabilityChanged =
        !m_rtHistoryValid ||
        (rtHasBrdfLut != m_prevRtHasBrdfLut) ||
        (rtHasIbl != m_prevRtHasIbl);

    const bool viewZReconsSettingsChanged =
        std::fabs(m_rtViewZReconsAlpha - m_prevRtViewZReconsAlpha) > 1e-6f;

    const bool viewZPolicyChanged =
        std::fabs(m_rtViewZSigmaScale - m_prevRtViewZSigmaScale) > 1e-6f;

    const bool temporalSettingsChanged =
        !m_rtHistoryValid ||
        (std::fabs(m_rtTemporalAlpha - m_prevRtTemporalAlpha) > 1e-6f) ||
        (std::fabs(m_rtTemporalAlphaResp - m_prevRtTemporalAlphaResp) > 1e-6f) ||
        (std::fabs(m_rtTemporalRoughnessSigma - m_prevRtTemporalRoughnessSigma) > 1e-6f) ||
        (std::fabs(m_rtTemporalRoughnessSigmaResp - m_prevRtTemporalRoughnessSigmaResp) > 1e-6f) ||
        (std::fabs(m_rtTemporalSpecDirSigma - m_prevRtTemporalSpecDirSigma) > 1e-6f) ||
        (std::fabs(m_rtTemporalSpecDirRoughCutoff - m_prevRtTemporalSpecDirRoughCutoff) > 1e-6f) ||
        (m_rtTemporalReprojectRadius != m_prevRtTemporalReprojectRadius) ||
        (std::fabs(m_rtTemporalReprojectMinConf - m_prevRtTemporalReprojectMinConf) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectLengthBias - m_prevRtHistorySelectLengthBias) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectLengthScale - m_prevRtHistorySelectLengthScale) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectLengthInfluence - m_prevRtHistorySelectLengthInfluence) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectThreshold - m_prevRtHistorySelectThreshold) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectRange - m_prevRtHistorySelectRange) > 1e-6f) ||
        (std::fabs(m_rtHistorySelectMotionTrustInfluence - m_prevRtHistorySelectMotionTrustInfluence) > 1e-6f) ||
        (std::fabs(m_rtTemporalVarianceScale - m_prevRtTemporalVarianceScale) > 1e-6f) ||
        (std::fabs(m_rtTemporalVarianceBias - m_prevRtTemporalVarianceBias) > 1e-6f) ||
        (std::fabs(m_rtTemporalVarianceAlphaBoost - m_prevRtTemporalVarianceAlphaBoost) > 1e-6f) ||
        (std::fabs(m_rtTemporalMotionConfMinDiffuse - m_prevRtTemporalMotionConfMinDiffuse) > 1e-6f) ||
        (std::fabs(m_rtTemporalMotionConfMinSpec - m_prevRtTemporalMotionConfMinSpec) > 1e-6f) ||
        (std::fabs(m_rtTemporalMotionConfPowerDiffuse - m_prevRtTemporalMotionConfPowerDiffuse) > 1e-6f) ||
        (std::fabs(m_rtTemporalMotionConfPowerSpec - m_prevRtTemporalMotionConfPowerSpec) > 1e-6f) ||
        viewZReconsSettingsChanged ||
        (m_rtTemporalEnableVarianceBoost != m_prevRtTemporalEnableVarianceBoost) ||
            viewZPolicyChanged;

    const bool resetRawAccumulation =
        cameraChanged ||
        drawListChanged ||
        debugViewChanged ||
        accumulationModeChanged ||
        integratorChanged ||
        iblAvailabilityChanged ||
        svgfChanged ||
        temporalSettingsChanged;

    const bool resetTemporalHistory =
        drawListStructuralChanged ||
        debugViewChanged ||
        accumulationModeChanged ||
        integratorChanged ||
        iblAvailabilityChanged ||
        bigCameraChanged ||
        !m_rtTemporal ||
        svgfChanged ||
        temporalSettingsChanged ||
        !m_prevRtMotionWorldsValid;

    if (resetRawAccumulation)
    {
        ResetRtAccumulation(resetTemporalHistory);
    }

    if (resetTemporalHistory)
    {
        m_rtTemporalHistoryValid = false;
    }

    if (drawListStructuralChanged ||
        bigCameraChanged ||
        !m_prevRtMotionWorldsValid ||
        viewZReconsSettingsChanged)
    {
        m_rtViewZHistoryValid = false;
    }

    // This must always be assigned before DXR dispatch and before post-stack gating.
    // RayGen receives this through PerFrameConstants::rtAccumulate.
    m_rtAccumulateThisFrame = allowRtAccumulation;

    if (wantsMotionDilateDebug ||
        wantsViewZReconstructDebug ||
        wantsDiffuseDemodDebug ||
        wantsOutlierClampDebug)
    {
        // Debug 54 is produced by RtMotionDilatePass.
        // DXR still runs to author raw motion/depth/normal guides, but it must not
        // advance progressive diffuse/spec accumulation while the compute debug
        // pass owns the final m_rtOutput image.
        m_rtAccumulateThisFrame = false;
        m_rtDispatchSampleIndex = 0;
    }

    // While validating the producer, the post stack is intentionally disabled.
    // Keep temporal history invalid so stale temporal/spec history cannot affect
    // the first frame after this gate is later reopened.
    if (rtPostMode == RtPostMode::Disabled ||
        rtPostMode == RtPostMode::RawCombineOnly)
    {
        m_rtTemporalHistoryValid = false;
        m_rtSurfaceIdHistoryValid = false;
    }

    m_prevRtCamYaw = m_camYaw;
    m_prevRtCamPitch = m_camPitch;
    m_prevRtCamRadius = m_camRadius;
    m_prevRtDebugView = m_debugView;
    m_prevRtHasBrdfLut = rtHasBrdfLut;
    m_prevRtHasIbl = rtHasIbl;
    m_rtAccumulatingLastFrame = m_rtAccumulateThisFrame;

    m_prevRtWorlds.resize(m_draws.size());
    m_prevRtMaterials.resize(m_draws.size());
    m_prevRtSvgf = m_rtSvgf;
    m_prevRtAtrousIterations = m_rtAtrousIterations;
    m_prevRtAtrousIterationsSpec = m_rtAtrousIterationsSpec;
    m_prevRtVarianceScale = m_rtVarianceScale;
    m_prevRtTemporalRoughnessSigma = m_rtTemporalRoughnessSigma;
    m_prevRtTemporalAlpha = m_rtTemporalAlpha;
    m_prevRtTemporalAlphaResp = m_rtTemporalAlphaResp;
    m_prevRtTemporalRoughnessSigmaResp = m_rtTemporalRoughnessSigmaResp;
    m_prevRtHistorySelectThreshold = m_rtHistorySelectThreshold;
    m_prevRtHistorySelectRange = m_rtHistorySelectRange;
    m_prevRtTemporalSpecDirSigma = m_rtTemporalSpecDirSigma;
    m_prevRtTemporalSpecDirRoughCutoff = m_rtTemporalSpecDirRoughCutoff;
    m_prevRtTemporalReprojectRadius = m_rtTemporalReprojectRadius;
    m_prevRtTemporalReprojectMinConf = m_rtTemporalReprojectMinConf;
    m_prevRtHistorySelectLengthBias = m_rtHistorySelectLengthBias;
    m_prevRtHistorySelectLengthScale = m_rtHistorySelectLengthScale;
    m_prevRtHistorySelectLengthInfluence = m_rtHistorySelectLengthInfluence;
    m_prevRtHistorySelectMotionTrustInfluence = m_rtHistorySelectMotionTrustInfluence;
    m_prevRtAtrousLengthAttenuation = m_rtAtrousLengthAttenuation;
    m_prevRtAtrousLengthPower = m_rtAtrousLengthPower;
    m_prevRtAtrousLengthSkipThreshold = m_rtAtrousLengthSkipThreshold;
    m_prevRtAtrousEnableLengthSkip = m_rtAtrousEnableLengthSkip;
    m_prevRtTemporalVarianceScale = m_rtTemporalVarianceScale;
    m_prevRtTemporalVarianceBias = m_rtTemporalVarianceBias;
    m_prevRtTemporalVarianceAlphaBoost = m_rtTemporalVarianceAlphaBoost;
    m_prevRtTemporalEnableVarianceBoost = m_rtTemporalEnableVarianceBoost;
    m_prevRtTemporalMotionConfMinDiffuse = m_rtTemporalMotionConfMinDiffuse;
    m_prevRtTemporalMotionConfMinSpec = m_rtTemporalMotionConfMinSpec;
    m_prevRtTemporalMotionConfPowerDiffuse = m_rtTemporalMotionConfPowerDiffuse;
    m_prevRtTemporalMotionConfPowerSpec = m_rtTemporalMotionConfPowerSpec;
    m_prevRtViewZReconsAlpha = m_rtViewZReconsAlpha;
    m_prevRtViewZSigmaScale = m_rtViewZSigmaScale;

    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        m_prevRtWorlds[i] = m_draws[i].world;
        m_prevRtMaterials[i] = m_draws[i].material;
    }
    m_prevRtEnableIndirect = m_rtEnableIndirect;
    m_prevRtIndirectScale = m_rtIndirectScale;

    m_rtHistoryValid = true;

    bool renderedWithDxr = false;
    const bool canRunDxr =
        m_useRaytracing &&
        m_dxrAvailable &&
        m_device5 &&
        m_rtPipeline.StateObject();

    if (canRunDxr)
    {
        CmdBeginEvent(cmdList, "DXR");
        EnsureRtOutputSize(width, height);
        EnsureRtInstanceData(frameIndex);
        UpdateRtGeometryTable(frameIndex);

        const bool canReuseAccumulatedOutput =
            m_rtAccumulateThisFrame &&
            m_rtOutputReady &&
            m_rtAccumDiffuseReady &&
            m_rtAccumSpecReady &&
            (m_rtSampleIndex >= m_rtMaxSamples);
        if (!canReuseAccumulatedOutput)
        {
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

                uint32_t dispatchCount = 1;
                if (m_rtAccumulateThisFrame)
                {
                    const uint32_t safeSamplesPerFrame =
                        (m_rtSamplesPerFrame < 1u) ? 1u :
                        (m_rtSamplesPerFrame > 8u) ? 8u :
                        m_rtSamplesPerFrame;

                    const uint32_t remaining = (m_rtSampleIndex < m_rtMaxSamples)
                        ? (m_rtMaxSamples - m_rtSampleIndex)
                        : 0u;

                    dispatchCount = std::min(safeSamplesPerFrame, remaining);
                    if (dispatchCount == 0)
                        dispatchCount = 1;

                    if (m_rtSampleIndex > 0)
                    {
                        D3D12_RESOURCE_BARRIER barriers[2]{};

                        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        barriers[0].UAV.pResource = m_rtAccumDiffuse.Get();

                        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        barriers[1].UAV.pResource = m_rtAccumSpec.Get();

                        cmd4->ResourceBarrier(2, barriers);
                    }
                }


                for (uint32_t i = 0; i < dispatchCount; ++i)
                {
                    m_rtDispatchSampleIndex = m_rtAccumulateThisFrame ? (m_rtSampleIndex + i) : 0u;

                    const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb =
                        UpdateGlobalConstants(frameIndex, width, height, sceneTime);

                    const D3D12_GPU_VIRTUAL_ADDRESS rtRayGenCb =
                        UpdateRtRayGenConstants(frameIndex);

                    cmd4->SetComputeRootConstantBufferView(2, perFrameCb);
                    cmd4->SetComputeRootConstantBufferView(4, rtRayGenCb);
                    cmd4->DispatchRays(&rays);

                    if (i + 1 < dispatchCount)
                    {
                        D3D12_RESOURCE_BARRIER barriers[3]{};

                        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        barriers[0].UAV.pResource = m_rtAccumDiffuse.Get();

                        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        barriers[1].UAV.pResource = m_rtAccumSpec.Get();

                        barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        barriers[2].UAV.pResource = m_rtOutput.Get();

                        cmd4->ResourceBarrier(3, barriers);
                    }
                }

                if (m_rtAccumulateThisFrame)
                    m_rtSampleIndex = std::min(m_rtSampleIndex + dispatchCount, m_rtMaxSamples);
                else
                    m_rtSampleIndex = 0;
            }
        }

        RunRtDenoiser(
            cl,
            frameIndex,
            device,
            width,
            height);

        // Restore all RT writeable resources to UAV state for the next frame
        // This must run regardless of whether the spatial stage used A-Trous,
        // denoise fallback, or no spatial filter.
        cl.Transition(m_rtAccumDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAccumSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovMotion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovMotionDilated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovViewZRaw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovSurfaceId.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtAovDiffuseAlbedo.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        
        if (m_rtDiffuseRobustInputReady)
        {
            cl.Transition(m_rtDiffuseRobustInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (m_rtSpecRobustInputReady)
        {
            cl.Transition(m_rtSpecRobustInput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (m_rtDiffuseDemodulatedReady)
        {
            cl.Transition(m_rtDiffuseDemodulated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (m_rtAovViewZReconsReady)
        {
            cl.Transition(m_rtAovViewZRecons.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (m_rtAovViewZReconsConfReady)
        {
            cl.Transition(m_rtAovViewZReconsConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (m_rtPostReady)
        {
            cl.Transition(m_rtPostDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.Transition(m_rtPostSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (m_rtSpecSelectedMomentsReady)
            {
                cl.Transition(m_rtSpecSelectedMoments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }
        cl.FlushBarriers();

        if (m_rtOutputReady)
        {
            cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl.Transition(backbufferResource, D3D12_RESOURCE_STATE_COPY_DEST);
            cl.FlushBarriers();

            cl.Get()->CopyResource(backbufferResource, m_rtOutput.Get());

            cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.FlushBarriers();

            renderedWithDxr = true;
        }
        CmdEndEvent(cmdList); //DXR
    }
    if (!renderedWithDxr)
    {
        m_rtDispatchSampleIndex = 0;
        m_rtAccumulateThisFrame = false;

        const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb =
            UpdateGlobalConstants(frameIndex, width, height, sceneTime);
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
    
    m_prevViewProj = m_currViewProj;
    m_prevRtCameraPos = m_currRtCameraPos;
    CommitRtMotionWorlds();
}

void Renderer::OnResize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_widthCached = width;
    m_heightCached = height;

    m_depthReady = false;
    m_gbufferReady = false;
    m_rtOutputReady = false;
    m_rtAovMotionReady = false;
    m_rtAovMotionDilatedReady = false;
    m_rtAovMotionConfReady = false;
    m_prevRtMotionWorldsValid = false;
    m_prevRtMotionWorlds.clear();
    m_rtSpecSelectedMomentsReady = false;
    m_rtAovViewZRawReady = false;
    m_rtAovViewZReconsReady = false;
    m_rtAovViewZReconsConfReady = false;
    m_rtViewZHistoryValid = false;
    m_rtAovSurfaceIdReady = false;
    m_rtAovDiffuseAlbedoReady = false;
    m_rtDiffuseDemodulatedReady = false;
    m_rtSurfaceIdHistoryValid = false;
    m_rtDiffuseRobustInputReady = false;
    m_rtSpecRobustInputReady = false;

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

    m_rtAccumDiffuseReady = false;
    m_rtAccumSpecReady = false;

    m_rtAovReady = false;

    CreateOrResizeGBuffers(device, width, height);
   
    if (m_dxrAvailable && m_device5)
    {
        CreateRtOutput(m_device5.Get(), width, height);
        CreateRtAccum(m_device5.Get(), width, height);
        CreateRtAovs(m_device5.Get(), width, height);
        CreateRtHistoryResources(m_device5.Get(), width, height);
        
        ResetRtAccumulation();
        m_rtTemporalHistoryValid = false;
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
    m_currRtCameraPos = cb->cameraPos;
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
    cb->rtSampleIndex = m_rtDispatchSampleIndex;
    cb->rtResetId = m_rtResetId;
    cb->rtAccumulate = m_rtAccumulateThisFrame ? 1u : 0u;
    cb->rtEnableIndirect = m_rtEnableIndirect ? 1u : 0u;
    cb->rtIndirectScale = m_rtIndirectScale;
    
    m_currViewProj = cb->viewProj;
    m_currInvViewProj = cb->invViewProj;

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
		CreateRtOutput(device, m_widthCached, m_heightCached); //Cached variables changed in OnResize
		CreateRtAccum(device, m_widthCached, m_heightCached);
        CreateRtAovs(m_device5.Get(), m_widthCached, m_heightCached);
        
        m_rtDenoisePass.Initialize(device, Paths::ShaderDir());
        m_rtMotionDilatePass.Initialize(device, Paths::ShaderDir());
        m_rtTemporalPass.Initialize(device, Paths::ShaderDir());
        CreateRtHistoryResources(m_device5.Get(), m_widthCached, m_heightCached);
        m_rtAtrousPass.Initialize(device, Paths::ShaderDir());
        m_rtHistorySelectPass.Initialize(device, Paths::ShaderDir());
        m_rtCombinePass.Initialize(device, Paths::ShaderDir());
        m_rtViewZReconstructPass.Initialize(device, Paths::ShaderDir());
        m_rtDiffuseDemodulatePass.Initialize(device, Paths::ShaderDir());
        m_rtOutlierClampPass.Initialize(device, Paths::ShaderDir());
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

    if (m_dxrAvailable && m_device5)
    {
        CreateRtFallbackTextures(device, cl, frameIndex);
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
        item.rtObjectId = 1u;
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
        item.rtObjectId = 2u;
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
        item.rtObjectId = 3u;
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
        item.rtObjectId = 4u;
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
        !m_rtOutput ||
        !m_rtAccumDiffuse ||
        !m_rtAccumSpec ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtPostDiffuse ||
        !m_rtPostSpec ||
        !m_rtOutputReady ||
        !m_rtAovMotion ||
        !m_rtAovMotionReady ||
        !m_rtAovMotionDilated ||
        !m_rtAovMotionDilatedReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !m_rtAccumDiffuseReady ||
        !m_rtAccumSpecReady ||
        !m_rtAovReady ||
        !m_rtPostReady ||
        !m_rtSpecSelectedMoments ||
        !m_rtSpecSelectedMomentsReady ||
        !m_rtAovViewZRaw ||
        !m_rtAovViewZRawReady ||
        (m_rtOutputWidth != width) ||
        !m_rtAovViewZRecons ||
        !m_rtAovViewZReconsReady ||
        !m_rtAovViewZReconsConf ||
        !m_rtAovViewZReconsConfReady ||
        !m_rtHistoryViewZ[0] ||
        !m_rtHistoryViewZ[1] ||
        !m_rtHistoryViewZConf[0] ||
        !m_rtHistoryViewZConf[1] ||
        !m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtAovDiffuseAlbedo ||
        !m_rtAovDiffuseAlbedoReady ||
        !m_rtDiffuseDemodulated ||
        !m_rtDiffuseDemodulatedReady ||
        !m_rtDiffuseRobustInput ||
        !m_rtDiffuseRobustInputReady ||
        !m_rtSpecRobustInput ||
        !m_rtSpecRobustInputReady ||
        (m_rtOutputHeight != height);

    if (sizeMismatch)
    {
        m_prevRtMotionWorldsValid = false;
        m_prevRtMotionWorlds.clear();

        CreateRtOutput(m_device5.Get(), width, height);
        CreateRtAccum(m_device5.Get(), width, height);
        CreateRtAovs(m_device5.Get(), width, height);
        CreateRtHistoryResources(m_device5.Get(), width, height);
     
        m_rtTemporalHistoryValid = false;
        m_rtSurfaceIdHistoryValid = false;
        ResetRtAccumulation();
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
		m_rtOutputUav = m_srvHeap.Allocate(kRtUavTableCount);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateUnorderedAccessView(m_rtOutput.Get(), nullptr, &uav, RtUavCpuAt(0));

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

        data.objectId = item.rtObjectId != 0u
            ? item.rtObjectId
            : static_cast<uint32_t>(i + 1u);

        data.baseColorFactor = mat ? mat->baseColorFactor : DirectX::XMFLOAT4(1, 1, 1, 1);
        data.metallic = mat ? mat->metallicFactor : 0.0f;
        data.roughness = mat ? mat->roughnessFactor : 0.5f;

        data.meshType = (item.mesh == &m_floor) ? 0u : 1u;
        data.materialId = GetRtMaterialId(item.material);
        
        const bool hasPrevMotionWorld =
            m_prevRtMotionWorldsValid &&
            i < m_prevRtMotionWorlds.size();

        data.prevObjectToWorld = hasPrevMotionWorld
            ? m_prevRtMotionWorlds[i]
            : item.world;
        
        dst[i] = data;
    }

    D3D12_RANGE writtenRange{ 0, SIZE_T(sizeof(RTInstanceData) * m_draws.size()) };
    frame.instanceDataUpload->Unmap(0, &writtenRange);
}

void Renderer::UpdateRtGeometryTable(uint32_t frameIndex)
{
    auto& frame = m_rtFrames[frameIndex];

    //static constexpr uint32_t kRtTableDescriptorCount =
    //    kRtGeometrySrvCount + (kRtTexturesPerMaterial * kMaxRtMaterials);

    if (!frame.geometryTable.IsValid())
        frame.geometryTable = m_srvHeap.Allocate(kRtSrvTableCount);

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

    
    // Clear every non-geometry slot to deterministic nulls first.
    // This covers:
    // - t6.. material textures
    // - t30..t32 IBL textures
    for (uint32_t slot = kRtGeometrySrvCount; slot < kRtSrvTableCount; ++slot)
    {
        CreateNullTexture2DSRV(m_device5.Get(), HandleAt(slot), DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    auto WriteMaterial = [&](uint32_t materialId, const Texture* base, const Texture* normal, const Texture* orm)
    {
        if (materialId >= kMaxRtMaterials)
            return;

        const uint32_t firstSlot =
            kRtGeometrySrvCount + materialId * kRtTexturesPerMaterial;

        const Texture& baseTex = (base && base->IsValid()) ? *base : m_rtFallbackWhiteTex;
        const Texture& normalTex = (normal && normal->IsValid()) ? *normal : m_rtFallbackFlatNormalTex;
        const Texture& ormTex = (orm && orm->IsValid()) ? *orm : m_rtFallbackOrmTex;

        WriteRtTextureSrv(m_device5.Get(), HandleAt(firstSlot + 0), baseTex);
        WriteRtTextureSrv(m_device5.Get(), HandleAt(firstSlot + 1), normalTex);
        WriteRtTextureSrv(m_device5.Get(), HandleAt(firstSlot + 2), ormTex);
    };

    // Stable explicit mapping:
    // 0 = floor
    // 1 = metal
    // 2 = matte
    // 3 = glossy

    WriteMaterial(
        kRtMaterialFloor,
        &m_albedoTex,
        &m_normalTex,
        &m_metalRoughTex);

    WriteMaterial(
        kRtMaterialMetal,
        m_metalBaseTex.IsValid() ? &m_metalBaseTex : &m_albedoTex,
        m_metalNormalTex.IsValid() ? &m_metalNormalTex : &m_normalTex,
        m_metalOrmTex.IsValid() ? &m_metalOrmTex : &m_metalRoughTex);

    WriteMaterial(
        kRtMaterialMatte,
        m_matteBaseTex.IsValid() ? &m_matteBaseTex : &m_albedoTex,
        m_matteNormalTex.IsValid() ? &m_matteNormalTex : &m_normalTex,
        m_matteOrmTex.IsValid() ? &m_matteOrmTex : &m_metalRoughTex);

    WriteMaterial(
        kRtMaterialGlossy,
        m_glossyBaseTex.IsValid() ? &m_glossyBaseTex : &m_albedoTex,
        m_glossyNormalTex.IsValid() ? &m_glossyNormalTex : &m_normalTex,
        m_glossyOrmTex.IsValid() ? &m_glossyOrmTex : &m_metalRoughTex);

    // Leave IBL slots null unless real textures are valid.
    if (m_brdfLutTex.IsValid())
        WriteRtTextureSrv(m_device5.Get(), HandleAt(kRtSrv_BrdfLut), m_brdfLutTex);

    if (m_iblDiffuseTex.IsValid())
        WriteRtTextureSrv(m_device5.Get(), HandleAt(kRtSrv_IblDiff), m_iblDiffuseTex);

    if (m_iblSpecularTex.IsValid())
        WriteRtTextureSrv(m_device5.Get(), HandleAt(kRtSrv_IblSpec), m_iblSpecularTex);

    m_rtMaterialCount = 4;
}

void Renderer::WriteRtTextureSrv(
    ID3D12Device* device,
    D3D12_CPU_DESCRIPTOR_HANDLE dst,
    const Texture& texture) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = texture.SrvFormat();
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = std::max(1u, texture.MipCount());

    device->CreateShaderResourceView(texture.Get(), &srv, dst);
}

uint32_t Renderer::GetRtMaterialId(const Material* material) const
{
    if (material == &m_floorMaterial)  return kRtMaterialFloor;
    if (material == &m_metalMaterial)  return kRtMaterialMetal;
    if (material == &m_matteMaterial)  return kRtMaterialMatte;
    if (material == &m_glossyMaterial) return kRtMaterialGlossy;

    // Current draw list only uses the four materials above.
    return kRtMaterialFloor;
}

void Renderer::CreateRtFallbackTextures(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex)
{
    if (!m_rtFallbackWhiteTex.IsValid())
    {
        static const uint8_t kWhite[4] = { 255, 255, 255, 255 };
        m_rtFallbackWhiteTex.CreateFromRGBA8Data(
            device,
            cl,
            m_upload,
            frameIndex,
            1,
            1,
            kWhite,
            true,
            L"RT Fallback White");
    }

    if (!m_rtFallbackFlatNormalTex.IsValid())
    {
        static const uint8_t kFlatNormal[4] = { 128, 128, 255, 255 };
        m_rtFallbackFlatNormalTex.CreateFromRGBA8Data(
            device,
            cl,
            m_upload,
            frameIndex,
            1,
            1,
            kFlatNormal,
            false,
            L"RT Fallback Flat Normal");
    }

    if (!m_rtFallbackOrmTex.IsValid())
    {
        static const uint8_t kOrm[4] = { 255, 255, 255, 255 };
        m_rtFallbackOrmTex.CreateFromRGBA8Data(
            device,
            cl,
            m_upload,
            frameIndex,
            1,
            1,
            kOrm,
            false,
            L"RT Fallback ORM");
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::RtUavCpuAt(uint32_t slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtOutputUav.cpu;
    h.ptr += SIZE_T(slot) * SIZE_T(m_srvHeap.DescriptorSize());
    return h;
}

void Renderer::ResetRtAccumulation(bool resetTemporalHistory)
{
    m_rtSampleIndex = 0;
    ++m_rtResetId;
    m_rtDispatchSampleIndex = 0;
    if (resetTemporalHistory)
    {
        m_rtTemporalHistoryValid = false;
        m_rtSurfaceIdHistoryValid = false;
    }

    m_rtHistoryValid = false;
}

void Renderer::CreateRtAccum(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtAccumDiffuseReady = false;
    m_rtAccumSpecReady = false;

    m_rtAccumDiffuse.Reset();
    m_rtAccumSpec.Reset();

    // DXR UAV table:
    // u0 = m_rtOutput
    // u1 = m_rtAccumDiffuse
    // u2 = m_rtAccumSpec
    // u3 = m_rtAovNormal
    // u4 = m_rtAovDepth
    if (!m_rtOutputUav.IsValid())
        m_rtOutputUav = m_srvHeap.Allocate(kRtUavTableCount);

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
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
            IID_PPV_ARGS(&m_rtAccumDiffuse)),
        "Create RT diffuse accumulation");

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_rtAccumSpec)),
        "Create RT specular accumulation");

    SetD3D12ObjectName(m_rtAccumDiffuse.Get(), L"RT Accum Diffuse");
    SetD3D12ObjectName(m_rtAccumSpec.Get(), L"RT Accum Spec");
    
    CommandList::SetGlobalState(
        m_rtAccumDiffuse.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CommandList::SetGlobalState(
        m_rtAccumSpec.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);


    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateUnorderedAccessView(
        m_rtAccumDiffuse.Get(),
        nullptr,
        &uav,
        RtUavCpuAt(1));

    device->CreateUnorderedAccessView(
        m_rtAccumSpec.Get(),
        nullptr,
        &uav,
        RtUavCpuAt(2));
    
    m_rtAccumDiffuseReady = true;
    m_rtAccumSpecReady = true;
}

void Renderer::CreateRtAovs(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtAovReady = false;
    m_rtAovMotionReady = false;
    m_rtAovNormal.Reset();
    m_rtAovDepth.Reset();
    m_rtAovMotion.Reset();
    m_rtAovMotionDilatedReady = false;
    m_rtAovMotionDilated.Reset();
    m_rtAovMotionConfReady = false;
    m_rtAovMotionConf.Reset();
    m_rtAovViewZRawReady = false;
    m_rtAovViewZRaw.Reset();
    m_rtAovViewZReconsReady = false;
    m_rtAovViewZReconsConfReady = false;
    m_rtAovViewZRecons.Reset();
    m_rtAovViewZReconsConf.Reset();
    m_rtAovSurfaceIdReady = false;
    m_rtAovSurfaceId.Reset();
    m_rtAovDiffuseAlbedoReady = false;
    m_rtAovDiffuseAlbedo.Reset();

    m_rtDiffuseDemodulatedReady = false;
    m_rtDiffuseDemodulated.Reset();

    m_rtDiffuseRobustInputReady = false;
    m_rtSpecRobustInputReady = false;
    m_rtDiffuseRobustInput.Reset();
    m_rtSpecRobustInput.Reset();

    if (!m_rtOutputUav.IsValid())
        m_rtOutputUav = m_srvHeap.Allocate(kRtUavTableCount);

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovNormal)),
            "Create RT AOV normal");

        SetD3D12ObjectName(m_rtAovNormal.Get(), L"RT AOV Normal");
        CommandList::SetGlobalState(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_rtAovNormal.Get(), nullptr, &uav, RtUavCpuAt(3));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovDepth)),
            "Create RT AOV depth");

        SetD3D12ObjectName(m_rtAovDepth.Get(), L"RT AOV Depth");
        CommandList::SetGlobalState(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_rtAovDepth.Get(), nullptr, &uav, RtUavCpuAt(4));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovMotion)),
            "Create RT AOV motion");

        SetD3D12ObjectName(m_rtAovMotion.Get(), L"RT AOV Motion PrevUV");

        CommandList::SetGlobalState(
            m_rtAovMotion.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovMotion.Get(),
            nullptr,
            &uav,
            RtUavCpuAt(5));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovMotionDilated)),
            "Create RT AOV motion dilated");

        SetD3D12ObjectName(
            m_rtAovMotionDilated.Get(),
            L"RT AOV Motion PrevUV Dilated");

        CommandList::SetGlobalState(
            m_rtAovMotionDilated.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovMotionConf)),
            "Create RT AOV motion confidence");

        SetD3D12ObjectName(
            m_rtAovMotionConf.Get(),
            L"RT AOV Motion Confidence");

        CommandList::SetGlobalState(
            m_rtAovMotionConf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovViewZRaw)),
            "Create RT AOV primary ViewZ Raw");

        SetD3D12ObjectName(
            m_rtAovViewZRaw.Get(),
            L"RT AOV Primary ViewZ Raw");

        CommandList::SetGlobalState(
            m_rtAovViewZRaw.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovViewZRaw.Get(),
            nullptr,
            &uav,
            RtUavCpuAt(6));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R32_UINT,
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
                IID_PPV_ARGS(&m_rtAovSurfaceId)),
            "Create RT AOV surface id");

        SetD3D12ObjectName(
            m_rtAovSurfaceId.Get(),
            L"RT AOV Surface ID");

        CommandList::SetGlobalState(
            m_rtAovSurfaceId.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovSurfaceId.Get(),
            nullptr,
            &uav,
            RtUavCpuAt(7));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                IID_PPV_ARGS(&m_rtAovDiffuseAlbedo)),
            "Create RT AOV diffuse albedo");

        SetD3D12ObjectName(
            m_rtAovDiffuseAlbedo.Get(),
            L"RT AOV Diffuse Albedo");

        CommandList::SetGlobalState(
            m_rtAovDiffuseAlbedo.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovDiffuseAlbedo.Get(),
            nullptr,
            &uav,
            RtUavCpuAt(8));
    }

    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                IID_PPV_ARGS(&m_rtDiffuseDemodulated)),
            "Create RT diffuse demodulated");

        SetD3D12ObjectName(
            m_rtDiffuseDemodulated.Get(),
            L"RT Diffuse Demodulated Lighting");

        CommandList::SetGlobalState(
            m_rtDiffuseDemodulated.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto CreateR16UavTexture = [&](ComPtr<ID3D12Resource>& resource, const wchar_t* name)
    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16_FLOAT,
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
                IID_PPV_ARGS(&resource)),
            "Create RT R16 UAV texture");

        SetD3D12ObjectName(resource.Get(), name);

        CommandList::SetGlobalState(
            resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };

    auto CreateRobustTexture = [&](ComPtr<ID3D12Resource>& resource, const wchar_t* name)
    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                IID_PPV_ARGS(&resource)),
            "Create RT robust input");

        SetD3D12ObjectName(resource.Get(), name);

        CommandList::SetGlobalState(
            resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };

    CreateRobustTexture(m_rtDiffuseRobustInput, L"RT Diffuse Robust Input");
    CreateRobustTexture(m_rtSpecRobustInput, L"RT Spec Robust Input");

    m_rtDiffuseRobustInputReady = true;
    m_rtSpecRobustInputReady = true;

    CreateR16UavTexture(
        m_rtAovViewZRecons,
        L"RT AOV ViewZ Reconstructed");

    CreateR16UavTexture(
        m_rtAovViewZReconsConf,
        L"RT AOV ViewZ Reconstructed Confidence");

    m_rtAovDiffuseAlbedoReady = true;
    m_rtDiffuseDemodulatedReady = true;
    m_rtAovSurfaceIdReady = true;
    m_rtAovViewZReconsReady = true;
    m_rtAovViewZReconsConfReady = true;
    m_rtAovViewZRawReady = true;
    m_rtAovMotionConfReady = true;
    m_rtAovMotionDilatedReady = true;
    m_rtAovMotionReady = true;
    m_rtAovReady = true;
}

bool Renderer::UpdateRtDenoiseSrvTable(
    uint32_t frameIndex,
    ID3D12Device* device,
    ID3D12Resource* signalResource,
    DescriptorAllocator::Allocation& table,
    uint32_t& tableSrvCount,
    ID3D12Resource* surfaceIdResource)
{
    if (!device ||
        !signalResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovMotionConf ||
        !m_rtAovReady ||
        !m_rtAovMotionConfReady)
    {
        return false;
    }

    if (!surfaceIdResource)
    {
        return false;
    }

    EnsureRtDescriptorTable(
        table,
        tableSrvCount,
        kRtDenoiseSrvCount);


    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = table.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    // t0 = input signal, linear HDR R16G16B16A16_FLOAT.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(signalResource, &srv, SrvAt(0));
    }

    // t1 = current normal/roughness guide.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_rtAovNormal.Get(), &srv, SrvAt(1));
    }

    // t2 = current depth guide.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_rtAovDepth.Get(), &srv, SrvAt(2));
    }

    // t3 = motion confidence.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotionConf.Get(),
            &srv,
            SrvAt(3));
    }

    // t4 = surface id
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            surfaceIdResource,
            &srv,
            SrvAt(4));
    }

    return true;
}

void Renderer::CreateRtHistoryResources(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtTemporalHistoryValid = false;
    m_rtHistoryReadIndex = 0;
    m_rtSurfaceIdHistoryValid = false;

    

    for (auto& r : m_rtHistoryAccum)  r.Reset();
    for (auto& r : m_rtHistoryNormal) r.Reset();
    for (auto& r : m_rtHistoryDepth)  r.Reset();
    for (auto& r : m_rtHistoryMoments) r.Reset();
    for (auto& r : m_rtSvgfPing) r.Reset();
    for (auto& r : m_rtHistorySpec)             r.Reset();
    for (auto& r : m_rtHistorySpecResp)         r.Reset();
    for (auto& r : m_rtHistoryMomentsSpec)      r.Reset();
    for (auto& r : m_rtHistoryMomentsSpecResp)  r.Reset();
    for (auto& r : m_rtHistorySurfaceId)        r.Reset();

    m_rtPostDiffuse.Reset();
    m_rtPostSpec.Reset();
    m_rtPostReady = false;

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

    for (uint32_t i = 0; i < 2; ++i)
    {
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistoryAccum[i])),
                "Create RT history accum");

            CommandList::SetGlobalState(m_rtHistoryAccum[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                width,
                height,
                1,
                1,
                1,
                0,
                D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_rtHistoryNormal[i])),
                "Create RT history normal");

            CommandList::SetGlobalState(m_rtHistoryNormal[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }

        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R32_FLOAT,
                width,
                height,
                1,
                1,
                1,
                0,
                D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&m_rtHistoryDepth[i])),
                "Create RT history depth");

            CommandList::SetGlobalState(m_rtHistoryDepth[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistoryMoments[i])),
                "Create RT history moments");

            CommandList::SetGlobalState(m_rtHistoryMoments[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtSvgfPing[i])),
                "Create RT SVGF ping");

            CommandList::SetGlobalState(m_rtSvgfPing[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            
        }
      
    
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistorySpec[i])),
                "Create RT history spec");

            CommandList::SetGlobalState(
                m_rtHistorySpec[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16B16A16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistorySpecResp[i])),
                "Create RT history spec responsive");

            CommandList::SetGlobalState(
                m_rtHistorySpecResp[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistoryMomentsSpec[i])),
                "Create RT history moments spec");

            CommandList::SetGlobalState(
                m_rtHistoryMomentsSpec[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16G16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistoryMomentsSpecResp[i])),
                "Create RT history moments spec responsive");

            CommandList::SetGlobalState(
                m_rtHistoryMomentsSpecResp[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        {
            m_rtHistoryViewZ[i].Reset();
            m_rtHistoryViewZConf[i].Reset();

            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R16_FLOAT,
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
                    IID_PPV_ARGS(&m_rtHistoryViewZ[i])),
                "Create RT history ViewZ");

            SetD3D12ObjectName(
                m_rtHistoryViewZ[i].Get(),
                i == 0 ? L"RT History ViewZ 0" : L"RT History ViewZ 1");

            CommandList::SetGlobalState(
                m_rtHistoryViewZ[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ThrowIfFailed(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr,
                    IID_PPV_ARGS(&m_rtHistoryViewZConf[i])),
                "Create RT history ViewZ confidence");

            SetD3D12ObjectName(
                m_rtHistoryViewZConf[i].Get(),
                i == 0 ? L"RT History ViewZ Confidence 0" : L"RT History ViewZ Confidence 1");

            CommandList::SetGlobalState(
                m_rtHistoryViewZConf[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        {
            auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R32_UINT,
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
                    IID_PPV_ARGS(&m_rtHistorySurfaceId[i])),
                "Create RT surface id history");

            SetD3D12ObjectName(
                m_rtHistorySurfaceId[i].Get(),
                i == 0
                ? L"RT History SurfaceId 0"
                : L"RT History SurfaceId 1");

            CommandList::SetGlobalState(
                m_rtHistorySurfaceId[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    m_rtViewZHistoryValid = false;
    UpdateRtSvgfPingUavTable(device);
    CreateRtPostResources(device, width, height);
}

void Renderer::CreateRtPostResources(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtPostReady = false;

    m_rtPostDiffuse.Reset();
    m_rtPostSpec.Reset();
    m_rtSpecSelectedMomentsReady = false;
    m_rtSpecSelectedMoments.Reset();

    if (!m_rtPostUavTable.IsValid())
        m_rtPostUavTable = m_srvHeap.Allocate(2);

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
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
            IID_PPV_ARGS(&m_rtPostDiffuse)),
        "Create RT post diffuse");

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_rtPostSpec)),
        "Create RT post spec");


    SetD3D12ObjectName(m_rtPostDiffuse.Get(), L"RT Post Diffuse");
    SetD3D12ObjectName(m_rtPostSpec.Get(), L"RT Post Spec");


    CommandList::SetGlobalState(
        m_rtPostDiffuse.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CommandList::SetGlobalState(
        m_rtPostSpec.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    {
        auto momentsDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_R16G16_FLOAT,
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
                &momentsDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(&m_rtSpecSelectedMoments)),
            "Create RT selected spec moments");

        SetD3D12ObjectName(
            m_rtSpecSelectedMoments.Get(),
            L"RT Selected Spec Moments");

        CommandList::SetGlobalState(
            m_rtSpecSelectedMoments.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    device->CreateUnorderedAccessView(
        m_rtPostDiffuse.Get(),
        nullptr,
        &uav,
        RtPostUavCpuAt(0));

    device->CreateUnorderedAccessView(
        m_rtPostSpec.Get(),
        nullptr,
        &uav,
        RtPostUavCpuAt(1));

    m_rtPostReady = true;
    m_rtSpecSelectedMomentsReady = true;

}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::RtPostUavCpuAt(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtPostUavTable.cpu;
    h.ptr += static_cast<SIZE_T>(index) * m_srvHeap.DescriptorSize();
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::RtPostUavGpuAt(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_rtPostUavTable.gpu;
    h.ptr += static_cast<UINT64>(index) * m_srvHeap.DescriptorSize();
    return h;
}

bool Renderer::UpdateRtCombineSrvTable(uint32_t frameIndex, ID3D12Device* device,
    ID3D12Resource* diffuseResource,
    ID3D12Resource* specularResource)
{
    if (!device ||
        !diffuseResource ||
        !specularResource ||
        !m_rtAovDiffuseAlbedo ||
        !m_rtAovDiffuseAlbedoReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.combineSrvTable,
        frame.combineSrvCount,
        kRtCombineSrvCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.combineSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto WriteRgba16Srv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    // t0 = diffuse lighting/radiance
    WriteRgba16Srv(diffuseResource, 0);

    // t1 = spec radiance
    WriteRgba16Srv(specularResource, 1);

    // t2 = diffuse albedo
    WriteRgba16Srv(m_rtAovDiffuseAlbedo.Get(), 2);

    return true;
}

bool Renderer::UpdateRtTemporalTables(
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
    uint32_t& uavTableCount)
{
    if (!device ||
        !currAccumResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !prevAccumResource ||
        !prevMomentsResource ||
        !m_rtHistoryNormal[0] ||
        !m_rtHistoryNormal[1] ||
        !m_rtHistoryDepth[0] ||
        !m_rtHistoryDepth[1] ||
        !m_rtAovMotionDilated ||
        !m_rtAovMotionDilatedReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !outAccumResource ||
        !outMomentsResource ||
        !currSurfaceIdResource ||
        !m_rtAccumDiffuseReady ||
        !m_rtAccumSpecReady ||
        !m_rtAovReady ||
        !m_rtOutput)
    {
        return false;
    }

    EnsureRtDescriptorTable(
        srvTable,
        srvTableCount,
        kRtTemporalSrvCount);

    EnsureRtDescriptorTable(
        uavTable,
        uavTableCount,
        kRtTemporalUavCount);

    const uint32_t readIndex = m_rtHistoryReadIndex;

    auto SrvAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    auto UavAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = uavTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    // t0 = current signal, t1 = current normal/roughness,
    // t3 = previous history signal, t4 = previous normal.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(currAccumResource, &srv, SrvAt(0));
        device->CreateShaderResourceView(m_rtAovNormal.Get(), &srv, SrvAt(1));
        device->CreateShaderResourceView(prevAccumResource, &srv, SrvAt(3));
        device->CreateShaderResourceView(m_rtHistoryNormal[readIndex].Get(), &srv, SrvAt(4));
    }

    // t2 = current depth, t5 = previous depth.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(m_rtAovDepth.Get(), &srv, SrvAt(2));
        device->CreateShaderResourceView(m_rtHistoryDepth[readIndex].Get(), &srv, SrvAt(5));
    }

    // t6 = previous moments.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(prevMomentsResource, &srv, SrvAt(6));
    }

    // t7 = dilated previous UV / motion AOV.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotionDilated.Get(),
            &srv,
            SrvAt(7));
    }

    // u0 = output history signal, u1 = debug/output, u2 = output moments.
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            outAccumResource,
            nullptr,
            &uav,
            UavAt(0));

        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        device->CreateUnorderedAccessView(
            m_rtOutput.Get(),
            nullptr,
            &uav,
            UavAt(1));

        uav.Format = DXGI_FORMAT_R16G16_FLOAT;

        device->CreateUnorderedAccessView(
            outMomentsResource,
            nullptr,
            &uav,
            UavAt(2));
    }

    // t8 = motion confidence.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotionConf.Get(),
            &srv,
            SrvAt(8));
    }

    // t9/t10/t11 = optional hit-distance guides.
    auto WriteR16Srv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    // t9 = current reconstructed ViewZ
    WriteR16Srv(currViewZResource, 9);

    // t10 = previous reconstructed ViewZ
    WriteR16Srv(prevViewZResource, 10);

    // t11 = previous reconstructed ViewZ confidence
    WriteR16Srv(prevViewZConfResource, 11);

    auto WriteR32UintSrv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    // t12 = current SurfaceId AOV
    WriteR32UintSrv(currSurfaceIdResource, 12);

    // t13 = previous SurfaceId history
    WriteR32UintSrv(prevSurfaceIdResource, 13);

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtTemporalConstants(
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
    float antiLagStrength)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtTemporalConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    
    // Variance bias is intentionally clamped to [-1, 1]. It is an offset after
    // Variance scaling, so wider values tend to saturate the debug/alpha response.

    auto* cb = reinterpret_cast<RtTemporalConstants*>(alloc.cpu);
    *cb = {};

    cb->currInvViewProj = m_currInvViewProj;
    cb->prevViewProj = m_prevViewProj;
    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };
    cb->temporalAlpha = temporalAlpha;
    cb->depthSigma = m_rtTemporalDepthSigma;
    cb->normalSigma = m_rtTemporalNormalSigma;
    cb->roughnessSigma = roughnessSigma;
    cb->specDirSigma = m_rtTemporalSpecDirSigma;
    cb->specDirRoughCutoff = m_rtTemporalSpecDirRoughCutoff;
    cb->temporalEnabled = m_rtTemporal ? 1u : 0u;
    cb->historyValid = m_rtTemporalHistoryValid ? 1u : 0u;
    cb->debugView = m_debugView;
    cb->reprojectRadius = std::min(m_rtTemporalReprojectRadius, 2u);
    cb->reprojectMinConf = std::clamp(m_rtTemporalReprojectMinConf, 0.0f, 1.0f);
    cb->varianceBias = std::clamp(m_rtTemporalVarianceBias, -1.0f, 1.0f);
    cb->varianceScale = std::max(0.0f, m_rtTemporalVarianceScale);
    cb->varianceAlphaBoost = std::max(0.0f, m_rtTemporalVarianceAlphaBoost);
    cb->enableVarianceBoost = m_rtTemporalEnableVarianceBoost ? 1u : 0u;
    cb->motionConfMin = std::clamp(motionConfMin, 0.0f, 1.0f);
    cb->motionConfPower = std::max(0.0f, motionConfPower);
    cb->viewZSigmaScale = std::max(0.0f, viewZSigmaScale);
    cb->viewZRoughCutoff = kRtViewZRoughCutoff;
    cb->viewZConfMin = kRtViewZConfMin;
    cb->surfaceIdHistoryValid = surfaceIdHistoryValid ? 1u : 0u;
    cb->distanceNormParams = RtDistanceNormParams();
    cb->distanceNormSigma = kRtDistanceNormSigma;
    cb->enableRobustMoments = m_rtEnableRobustMoments ? 1u : 0u;

    const float maxLum =
        std::max(1e-3f, m_rtOutlierClampMaxLuminance);

    cb->momentLuminanceMax = maxLum;
    cb->momentVarianceMax = maxLum * maxLum;
    cb->historyClampStrength = std::clamp(historyClampStrength, 0.0f, 1.0f);

    cb->temporalNeighborhoodSigmaK = m_rtTemporalNeighborhoodSigmaK;
    cb->temporalClampMinWeight = m_rtTemporalClampMinWeight;
    cb->temporalClampRelaxation = m_rtTemporalClampRelaxation;

    cb->currCameraPos = 
    {
        m_currRtCameraPos.x,
        m_currRtCameraPos.y,
        m_currRtCameraPos.z,
        0.0f
    };

    cb->prevCameraPos = 
    {
        m_prevRtCameraPos.x,
        m_prevRtCameraPos.y,
        m_prevRtCameraPos.z,
        0.0f
    };

    cb->enableSignalConfidence = m_rtEnableSignalConfidence ? 1u : 0u;
    cb->signalDeltaSigma = std::max(1e-4f, signalDeltaSigma);
    cb->confidencePower = std::max(1e-4f, confidencePower);
    cb->minSignalConfidence = std::clamp(m_rtMinSignalConfidence, 0.0f, 1.0f);

    cb->antiLagStrength = std::clamp(antiLagStrength, 0.0f, 1.0f);
    cb->varianceConfidenceScale = std::max(0.0f, m_rtVarianceConfidenceScale);
    cb->historyLengthConfidencePower = std::max(1e-4f, m_rtHistoryLengthConfidencePower);
    cb->responsiveAlphaBoost = std::max(0.0f, m_rtResponsiveAlphaBoost);

    cb->maxStableHistory = std::clamp(m_rtMaxStableHistory, 1.0f, 255.0f);
    cb->minStableHistoryForClamp = std::max(1.0f, m_rtMinStableHistoryForClamp);
    cb->confidenceDebugScale = std::max(1e-4f, m_rtConfidenceDebugScale);
    cb->padShape0 = 0.0f;


    return alloc.gpu;
}

bool Renderer::UpdateRtSvgfSrvTable(
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
    ID3D12Resource* surfaceIdResource)
{
    if (!device ||
        !signalResource ||
        !m_rtAovNormal || !m_rtAovDepth ||
        !momentsResource ||
        !m_rtAovReady ||
        !motionConfResource ||
        !m_rtAovMotionConfReady ||
        iter >= kMaxRtAtrousIterations)
    {
        return false;
    }

    if (!surfaceIdResource)
    {
        return false;
    }

    EnsureRtDescriptorTable(
        table,
        tableSrvCount,
        kRtSvgfSrvCount);

    auto HandleAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = table.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(signalResource, &srv, HandleAt(0));
        device->CreateShaderResourceView(m_rtAovNormal.Get(), &srv, HandleAt(1));
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(m_rtAovDepth.Get(), &srv, HandleAt(2));
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(momentsResource, &srv, HandleAt(3));
    }

    // t4 = motion confidence
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            motionConfResource,
            &srv,
            HandleAt(4)); 
    }

    auto WriteR16Srv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        // Null descriptor is valid when hit-distance weighting is disabled.
        device->CreateShaderResourceView(
            resource,
            &srv,
            HandleAt(slot));
    };

    // t5 = reconstructed ViewZ
    WriteR16Srv(viewZResource, 5);

    // t6 = reconstructed ViewZ confidence
    WriteR16Srv(viewZConfResource, 6);

    // t7 = current surface id
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            surfaceIdResource,
            &srv,
            HandleAt(7));
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtAtrousConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height,
    uint32_t iterationIndex,
    bool useMoments,
    bool finalOutputSrgb,
    float motionConfPower,
    float motionConfMin,
    float viewZSigmaScale)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtAtrousConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    auto* cb = reinterpret_cast<RtAtrousConstants*>(alloc.cpu);
    *cb = {};

    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };
    cb->iterationIndex = iterationIndex;
    cb->stepWidth = 1u << iterationIndex;
    cb->sigmaDepth = m_rtDenoiseSigmaDepth;
    cb->sigmaNormal = m_rtDenoiseSigmaNormal;
    cb->varianceScale = m_rtVarianceScale;
    cb->useMoments = useMoments ? 1u : 0u;
    cb->finalOutputSrgb = finalOutputSrgb ? 1u : 0u;
    cb->lengthAttenuation = std::max(0.0f, std::min(1.0f, m_rtAtrousLengthAttenuation));
    cb->lengthPower = std::max(1e-4f, m_rtAtrousLengthPower);
    cb->motionConfPower = std::max(0.0f, motionConfPower);
    cb->motionConfMin = std::max(0.0f, std::min(1.0f, motionConfMin));
    cb->viewZSigmaScale = std::max(0.0f, viewZSigmaScale);
    cb->viewZRoughCutoff = kRtViewZRoughCutoff;
    cb->viewZConfMin = kRtViewZConfMin;
    cb->debugView = m_debugView;
    cb->lengthSkipThreshold = std::clamp(m_rtAtrousLengthSkipThreshold, 0.0f, 1.0f);
    cb->enableLengthSkip = m_rtAtrousEnableLengthSkip ? 1u : 0u;
    cb->distanceNormParams = RtDistanceNormParams();
    cb->distanceNormSigma = kRtDistanceNormSigma;
    cb->atrousContributionMaxLuminance = std::max(1e-3f, m_rtAtrousContributionMaxLuminance);

    const bool specSignal = motionConfPower > 0.0f;
    cb->enableAdaptiveAtrous = m_rtEnableAdaptiveAtrous ? 1u : 0u;

    cb->adaptiveVarianceScale =
        std::max(
            0.0f,
            specSignal
            ? m_rtAdaptiveVarianceScaleSpec
            : m_rtAdaptiveVarianceScaleDiffuse);

    cb->adaptiveHistoryMin = std::max(0.0f, m_rtAdaptiveHistoryMin);
    cb->adaptiveHistoryMax =
        std::max(cb->adaptiveHistoryMin + 1e-4f, m_rtAdaptiveHistoryMax);

    cb->diffuseBlurBoost = std::max(0.0f, m_rtDiffuseBlurBoost);
    cb->specBlurRoughnessBoost = std::max(0.0f, m_rtSpecBlurRoughnessBoost);
    cb->specGlossyBlurLimit = std::clamp(m_rtSpecGlossyBlurLimit, 1e-4f, 1.0f);
    cb->wideIterationConfidenceMin = std::clamp(m_rtWideIterationConfidenceMin, 0.0f, 1.0f);

    cb->adaptiveSigmaLMin = std::max(1e-4f, m_rtAdaptiveSigmaLMin);
    cb->adaptiveSigmaLMax =
        std::max(cb->adaptiveSigmaLMin, m_rtAdaptiveSigmaLMax);

    cb->adaptiveNormalRelaxation = std::max(0.0f, m_rtAdaptiveNormalRelaxation);
    cb->padAtrousShape0 = 0.0f;

    return alloc.gpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::RtSvgfPingUavGpuAt(uint32_t i) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_rtSvgfPingUavTable.gpu;
    h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
    return h;
}

void Renderer::UpdateRtSvgfPingUavTable(ID3D12Device* device)
{
    if (!device || !m_rtSvgfPing[0] || !m_rtSvgfPing[1])
        return;

    if (!m_rtSvgfPingUavTable.IsValid())
        m_rtSvgfPingUavTable = m_srvHeap.Allocate(2);

    auto HandleAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtSvgfPingUavTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    for (uint32_t i = 0; i < 2; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(m_rtSvgfPing[i].Get(), nullptr, &uav, HandleAt(i));
    }
}

bool Renderer::UpdateRtHistorySelectTables(
    uint32_t frameIndex,
    ID3D12Device* device,
    ID3D12Resource* stableSignal,
    ID3D12Resource* responsiveSignal,
    ID3D12Resource* stableMoments,
    ID3D12Resource* responsiveMoments)
{
    auto& frame = m_rtFrames[frameIndex];

    if (!device ||
        !stableSignal ||
        !responsiveSignal ||
        !stableMoments ||
        !responsiveMoments ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtSvgfPing[0] ||
        !m_rtSpecSelectedMoments ||
        !m_rtSpecSelectedMomentsReady ||
        !m_rtOutput ||
        !m_rtOutputReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !m_rtAovReady)
    {
        return false;
    }

    EnsureRtDescriptorTable(
        frame.historySelectSrvTable,
        frame.historySelectSrvCount,
        kRtHistorySelectSrvCount);

    EnsureRtDescriptorTable(
        frame.historySelectUavTable,
        frame.historySelectUavCount,
        kRtHistorySelectUavCount);

    auto SrvAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.historySelectSrvTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    auto UavAt = [&](uint32_t i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.historySelectUavTable.cpu;
        h.ptr += SIZE_T(i) * SIZE_T(m_srvHeap.DescriptorSize());
        return h;
    };

    // t0 = stable spec history
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            stableSignal,
            &srv,
            SrvAt(0));
    }

    // t1 = responsive spec history
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            responsiveSignal,
            &srv,
            SrvAt(1));
    }

    // t2 = stable spec moments
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            stableMoments,
            &srv,
            SrvAt(2));
    }

    // t3 = responsive spec moments
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            responsiveMoments,
            &srv,
            SrvAt(3));
    }

    // t4 = guide normal/roughness
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovNormal.Get(),
            &srv,
            SrvAt(4));
    }

    // t5 = guide depth
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovDepth.Get(),
            &srv,
            SrvAt(5));
    }

    // t6 = motion confidence
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotionConf.Get(),
            &srv,
            SrvAt(6));
    }

    // u0 = selected spec signal
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtSvgfPing[0].Get(),
            nullptr,
            &uav,
            UavAt(0));
    }

    // u1 = debug/display output
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtOutput.Get(),
            nullptr,
            &uav,
            UavAt(1));
    }

    // u2 = selected spec moments
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtSpecSelectedMoments.Get(),
            nullptr,
            &uav,
            UavAt(2));
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtHistorySelectConstants(uint32_t frameIndex)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtHistorySelectConstants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    
    auto* cb = reinterpret_cast<RtHistorySelectConstants*>(alloc.cpu);
    *cb = {};   

    cb->roughnessThreshold = m_rtHistorySelectThreshold;
    cb->roughnessRange = std::max(1e-4f, m_rtHistorySelectRange);

    cb->lengthBias = m_rtHistorySelectLengthBias;
    cb->lengthScale = std::max(0.0f, m_rtHistorySelectLengthScale);
    cb->lengthInfluence =
        std::max(0.0f, std::min(1.0f, m_rtHistorySelectLengthInfluence));

    cb->motionTrustInfluence =
        std::max(0.0f, std::min(1.0f, m_rtHistorySelectMotionTrustInfluence));

    // Reuse the spec temporal confidence policy so selection and spec temporal
    // agree on what "trusted motion" means. This intentionally means
    // m_rtTemporalMotionConfPowerSpec shapes both spec temporal reuse and
    // spec history selection; do not add a second selector power knob unless
    // tuning proves they must diverge.
    cb->motionConfMin =
        std::max(0.0f, std::min(1.0f, m_rtTemporalMotionConfMinSpec));

    cb->motionConfPower =
        std::max(1e-3f, m_rtTemporalMotionConfPowerSpec);

    cb->debugView = m_debugView;
    cb->pad0[0] = 0u;
    cb->pad0[1] = 0u;
    cb->pad0[2] = 0u;

    cb->enableSpecHistoryShaping = m_rtEnableSpecHistoryShaping ? 1u : 0u;
    cb->specResponsiveRoughnessCutoff =
        std::clamp(m_rtSpecResponsiveRoughnessCutoff, 1e-4f, 1.0f);

    cb->specResponsiveVarianceScale =
        std::max(0.0f, m_rtSpecResponsiveVarianceScale);

    cb->specResponsiveMotionScale =
        std::max(0.0f, m_rtSpecResponsiveMotionScale);

    cb->specStableMinHistory =
        std::max(1.0f, m_rtSpecStableMinHistory);

    cb->specResponsiveBias =
        std::clamp(m_rtSpecResponsiveBias, 0.0f, 1.0f);

    cb->specHistoryBlendPower =
        std::max(1e-4f, m_rtSpecHistoryBlendPower);

    cb->padSpecShape0 = 0.0f;

    return alloc.gpu;
}

bool Renderer::RunRtDenoiseSignal(
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
    ID3D12Resource* surfaceIdResource)
{
    if (!device ||
        !inputSignal ||
        !outputResource ||
        !surfaceIdResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !m_rtPostReady)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    CmdBeginEvent(cmdList, eventName);

    cl.Transition(inputSignal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(outputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(surfaceIdResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    const bool okTable =
        UpdateRtDenoiseSrvTable(
            frameIndex,
            device,
            inputSignal,
            srvTable,
            srvTableCount,
            surfaceIdResource);

    if (!okTable)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    m_rtDenoisePass.Dispatch(
        cl,
        srvTable.gpu,
        outputUav,
        width,
        height,
        m_rtDenoiseRadius,
        m_rtDenoiseSigmaDepth,
        m_rtDenoiseSigmaNormal,
        std::max(0.0f, std::min(1.0f, motionConfMin)),
        std::max(1e-3f, motionConfPower));

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = outputResource;
    cmdList->ResourceBarrier(1, &barrier);

    CmdEndEvent(cmdList);
    return true;
}

void Renderer::CommitRtMotionWorlds()
{
    m_prevRtMotionWorlds.resize(m_draws.size());

    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        m_prevRtMotionWorlds[i] = m_draws[i].world;
    }

    m_prevRtMotionWorldsValid = true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtRayGenConstants(uint32_t frameIndex)
{
    auto alloc = m_upload.Allocate(frameIndex, sizeof(RtRayGenConstants), 256);
    auto* cb = reinterpret_cast<RtRayGenConstants*>(alloc.cpu);

    cb->prevViewProj = m_prevViewProj;
    cb->currViewProj = m_currViewProj;
    cb->prevCameraPos = DirectX::XMFLOAT4(
        m_prevRtCameraPos.x,
        m_prevRtCameraPos.y,
        m_prevRtCameraPos.z,
        1.0f);
    cb->currCameraPos = DirectX::XMFLOAT4(
        m_currRtCameraPos.x,
        m_currRtCameraPos.y,
        m_currRtCameraPos.z,
        1.0f);
    
    cb->hasPrevMotion = m_prevRtMotionWorldsValid ? 1u : 0u;
    cb->pad0[0] = 0u;
    cb->pad0[1] = 0u;
    cb->pad0[2] = 0u;

    return alloc.gpu;
}

bool Renderer::UpdateRtMotionDilateTables(
    uint32_t frameIndex,
    ID3D12Device* device,
    bool useReconstructedViewZ)
{
    ID3D12Resource* viewZResource =
        (useReconstructedViewZ &&
            m_rtAovViewZRecons &&
            m_rtAovViewZReconsReady)
        ? m_rtAovViewZRecons.Get()
        : m_rtAovViewZRaw.Get();

    if (!device ||
        !m_rtAovMotion ||
        !m_rtAovMotionDilated ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtOutput ||
        !m_rtAovMotionReady ||
        !m_rtAovMotionDilatedReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !m_rtAovReady ||
        !m_rtAovViewZRaw ||
        !m_rtAovViewZRawReady ||
        !viewZResource ||
        !m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.motionDilateSrvTable,
        frame.motionDilateSrvCount,
        kRtMotionDilateSrvCount);

    EnsureRtDescriptorTable(
        frame.motionDilateUavTable,
        frame.motionDilateUavCount,
        kRtMotionDilateUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.motionDilateSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.motionDilateUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    // t0 = raw prevUV
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotion.Get(),
            &srv,
            SrvAt(0));
    }

    // t1 = normal/roughness guide
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovNormal.Get(),
            &srv,
            SrvAt(1));
    }

    // t2 = depth guide
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovDepth.Get(),
            &srv,
            SrvAt(2));
    }

    // t3 =  viewZ distance
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            viewZResource,
            &srv,
            SrvAt(3));
    }

    // t4 = surface id
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovSurfaceId.Get(),
            &srv,
            SrvAt(4));
    }

    // u0 = dilated prevUV
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovMotionDilated.Get(),
            nullptr,
            &uav,
            UavAt(0));
    }
    // u1 = motion confidence
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovMotionConf.Get(),
            nullptr,
            &uav,
            UavAt(1));
    }
    // u2 = debug/display output
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtOutput.Get(),
            nullptr,
            &uav,
            UavAt(2));
    }

    return true;
}

bool Renderer::RunRtMotionDilate(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    bool useReconstructedViewZ)
{
    ID3D12Resource* viewZResource =
        (useReconstructedViewZ &&
            m_rtAovViewZRecons &&
            m_rtAovViewZReconsReady)
        ? m_rtAovViewZRecons.Get()
        : m_rtAovViewZRaw.Get();

    if (!device ||
        !m_rtAovMotion ||
        !m_rtAovMotionDilated ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtOutput ||
        !m_rtAovMotionReady ||
        !m_rtAovMotionDilatedReady ||
        !m_rtAovMotionConf ||
        !m_rtAovMotionConfReady ||
        !m_rtAovReady ||
        !m_rtAovViewZRaw ||
        !m_rtAovViewZRawReady ||
        !viewZResource || 
        !m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT Motion Dilate");

    cl.Transition(m_rtAovMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovMotionDilated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(viewZResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovSurfaceId.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    const bool okTables =
        UpdateRtMotionDilateTables(
            frameIndex,
            device,
            useReconstructedViewZ);

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtMotionDilateConstants(frameIndex, width, height);

    m_rtMotionDilatePass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].motionDilateSrvTable.gpu,
        m_rtFrames[frameIndex].motionDilateUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[3]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_rtAovMotionDilated.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rtAovMotionConf.Get();

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(3, barriers);

    CmdEndEvent(cmdList);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtMotionDilateConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    constexpr uint32_t cbSize =
        (sizeof(RtMotionDilateConstants) + 255) & ~255;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<RtMotionDilateConstants*>(alloc.cpu);

    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };

    cb->radius = std::min(m_rtMotionDilateRadius, kMaxRtMotionDilateRadius);
    cb->depthSigma = m_rtMotionDilateDepthSigma;
    cb->normalSigma = m_rtMotionDilateNormalSigma;
    cb->minScore = m_rtMotionDilateMinScore;
    cb->debugView = m_debugView;
    cb->pad0 = 0;
    cb->distanceNormParams = RtDistanceNormParams();
    cb->distanceNormSigma = kRtDistanceNormSigma;

    return alloc.gpu;
}

bool Renderer::UpdateRtViewZReconstructTables(
    uint32_t frameIndex,
    ID3D12Device* device)
{
    if (!device ||
        !m_rtAovViewZRaw ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovMotion ||
        !m_rtHistoryViewZ[0] ||
        !m_rtHistoryViewZ[1] ||
        !m_rtHistoryDepth[0] ||
        !m_rtHistoryDepth[1] ||
        !m_rtHistoryNormal[0] ||
        !m_rtHistoryNormal[1] ||
        !m_rtAovViewZRecons ||
        !m_rtAovViewZReconsConf ||
        !m_rtOutput ||
        !m_rtAovViewZRawReady ||
        !m_rtAovMotionReady ||
        !m_rtAovViewZReconsReady ||
        !m_rtAovViewZReconsConfReady ||
        !m_rtAovReady ||
        !m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.viewZReconstructSrvTable,
        frame.viewZReconstructSrvCount,
        kRtViewZReconstructSrvCount);

    EnsureRtDescriptorTable(
        frame.viewZReconstructUavTable,
        frame.viewZReconstructUavCount,
        kRtViewZReconstructUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.viewZReconstructSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.viewZReconstructUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto WriteR16Srv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    auto WriteR16Uav = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            UavAt(slot));
    };

    // t0 = raw primary ViewZ
    WriteR16Srv(m_rtAovViewZRaw.Get(), 0);

    // t1 = current normal/roughness
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovNormal.Get(),
            &srv,
            SrvAt(1));
    }

    // t2 = current depth
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovDepth.Get(),
            &srv,
            SrvAt(2));
    }

    // t3 = raw prevUV
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovMotion.Get(),
            &srv,
            SrvAt(3));
    }

    const uint32_t readIndex = m_rtHistoryReadIndex;

    // t4 = previous reconstructed ViewZ
    WriteR16Srv(m_rtHistoryViewZ[readIndex].Get(), 4);

    // t5 = previous depth
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtHistoryDepth[readIndex].Get(),
            &srv,
            SrvAt(5));
    }

    // t6 = previous normal/roughness
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtHistoryNormal[readIndex].Get(),
            &srv,
            SrvAt(6));
    }

    // t7 = surface id
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovSurfaceId.Get(),
            &srv,
            SrvAt(7));
    }

    // u0 = reconstructed ViewZ
    WriteR16Uav(m_rtAovViewZRecons.Get(), 0);

    // u1 = reconstructed confidence
    WriteR16Uav(m_rtAovViewZReconsConf.Get(), 1);

    // u2 = debug/display output
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtOutput.Get(),
            nullptr,
            &uav,
            UavAt(2));
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtViewZReconstructConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtViewZReconstructConstants),
        256);

    auto* cb = reinterpret_cast<RtViewZReconstructConstants*>(alloc.cpu);
    *cb = {};

    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };

    cb->alpha =
        std::max(0.0f, std::min(1.0f, m_rtViewZReconsAlpha));

    cb->depthSigma = 0.02f;
    cb->normalSigma = 0.25f;
    cb->roughnessSigma = 0.20f;

    cb->historyValid = m_rtViewZHistoryValid ? 1u : 0u;
    cb->debugView = m_debugView;
    cb->radius = 2u;
    cb->viewZVisMax = 25.0f;
    cb->distanceNormParams = RtDistanceNormParams();
    cb->distanceNormSigma = kRtDistanceNormSigma;

    return alloc.gpu;
}

bool Renderer::RunRtViewZReconstruct(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height)
{
    if (!device ||
        !m_rtAovViewZRaw ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovMotion ||
        !m_rtAovViewZRecons ||
        !m_rtAovViewZReconsConf ||
        !m_rtOutput ||
        !m_rtAovViewZRawReady ||
        !m_rtAovMotionReady ||
        !m_rtAovReady ||
        !m_rtAovViewZReconsReady ||
        !m_rtAovViewZReconsConfReady ||
        !m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT ViewZ Reconstruct");

    cl.Transition(m_rtAovViewZRaw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryViewZ[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryDepth[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryNormal[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovViewZRecons.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtAovViewZReconsConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtAovSurfaceId.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    const bool okTables =
        UpdateRtViewZReconstructTables(
            frameIndex,
            device);

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtViewZReconstructConstants(
            frameIndex,
            width,
            height);

    m_rtViewZReconstructPass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].viewZReconstructSrvTable.gpu,
        m_rtFrames[frameIndex].viewZReconstructUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[3]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_rtAovViewZRecons.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rtAovViewZReconsConf.Get();

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(3, barriers);

    CmdEndEvent(cmdList);
    return true;
}

void Renderer::CommitRtViewZHistory(
    CommandList& cl,
    uint32_t writeIndex)
{
    if (writeIndex >= 2 ||
        !m_rtAovViewZRecons ||
        !m_rtAovViewZReconsConf ||
        !m_rtHistoryViewZ[writeIndex] ||
        !m_rtHistoryViewZConf[writeIndex] ||
        !m_rtAovViewZReconsReady ||
        !m_rtAovViewZReconsConfReady)
    {
        m_rtViewZHistoryValid = false;
        return;
    }

    auto* cmdList = cl.Get();

    cl.Transition(m_rtAovViewZRecons.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl.Transition(m_rtAovViewZReconsConf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl.Transition(m_rtHistoryViewZ[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    cl.Transition(m_rtHistoryViewZConf[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    cl.FlushBarriers();

    cmdList->CopyResource(
        m_rtHistoryViewZ[writeIndex].Get(),
        m_rtAovViewZRecons.Get());

    cmdList->CopyResource(
        m_rtHistoryViewZConf[writeIndex].Get(),
        m_rtAovViewZReconsConf.Get());

    cl.Transition(m_rtHistoryViewZ[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryViewZConf[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    m_rtViewZHistoryValid = true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtDiffuseDemodulateConstants(
    uint32_t frameIndex)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtDiffuseDemodulateConstants),
        256);

    auto* cb = reinterpret_cast<RtDiffuseDemodulateConstants*>(alloc.cpu);
    *cb = {};
    cb->debugView = m_debugView;

    return alloc.gpu;
}

bool Renderer::UpdateRtDiffuseDemodulateTables(
    uint32_t frameIndex,
    ID3D12Device* device)
{
    if (!device ||
        !m_rtAccumDiffuse ||
        !m_rtAovDiffuseAlbedo ||
        !m_rtAovDepth ||
        !m_rtDiffuseDemodulated ||
        !m_rtOutput ||
        !m_rtAccumDiffuseReady ||
        !m_rtAovDiffuseAlbedoReady ||
        !m_rtAovReady ||
        !m_rtDiffuseDemodulatedReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];


    EnsureRtDescriptorTable(
        frame.diffuseDemodSrvTable,
        frame.diffuseDemodSrvCount,
        kRtDiffuseDemodSrvCount);

    EnsureRtDescriptorTable(
        frame.diffuseDemodUavTable,
        frame.diffuseDemodUavCount,
        kRtDiffuseDemodUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.diffuseDemodSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.diffuseDemodUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto WriteRgba16Srv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    // t0 = diffuse radiance
    WriteRgba16Srv(m_rtAccumDiffuse.Get(), 0);

    // t1 = diffuse albedo
    WriteRgba16Srv(m_rtAovDiffuseAlbedo.Get(), 1);

    // t2 = depth
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            m_rtAovDepth.Get(),
            &srv,
            SrvAt(2));
    }

    // u0 = demodulated diffuse lighting
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtDiffuseDemodulated.Get(),
            nullptr,
            &uav,
            UavAt(0));
    }

    // u1 = debug/display output
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtOutput.Get(),
            nullptr,
            &uav,
            UavAt(1));
    }

    return true;
}

bool Renderer::RunRtDiffuseDemodulate(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height)
{
    if (!device ||
        !m_rtAccumDiffuse ||
        !m_rtAovDiffuseAlbedo ||
        !m_rtAovDepth ||
        !m_rtDiffuseDemodulated ||
        !m_rtOutput ||
        !m_rtAccumDiffuseReady ||
        !m_rtAovDiffuseAlbedoReady ||
        !m_rtAovReady ||
        !m_rtDiffuseDemodulatedReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT Diffuse Demodulate");

    cl.Transition(m_rtAccumDiffuse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDiffuseAlbedo.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtDiffuseDemodulated.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.FlushBarriers();

    const bool okTables =
        UpdateRtDiffuseDemodulateTables(
            frameIndex,
            device);

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtDiffuseDemodulateConstants(frameIndex);

    m_rtDiffuseDemodulatePass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].diffuseDemodSrvTable.gpu,
        m_rtFrames[frameIndex].diffuseDemodUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[2]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_rtDiffuseDemodulated.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(2, barriers);

    CmdEndEvent(cmdList);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtCombineConstants(
    uint32_t frameIndex,
    bool diffuseIsDemodulated)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtCombineConstants),
        256);

    auto* cb = reinterpret_cast<RtCombineConstants*>(alloc.cpu);
    *cb = {};
    cb->diffuseIsDemodulated = diffuseIsDemodulated ? 1u : 0u;

    return alloc.gpu;
}

Renderer::RtDebugRouting Renderer::BuildRtDebugRouting(uint32_t debugView) const
{
    RtDebugRouting r{};
    r.debugView = debugView;

    r.wantsTemporalDebug = IsTemporalDebug(debugView);
    r.wantsSvgfDebug = IsSvgfDebug(debugView);
    r.wantsAtrousOutputDebug =
        debugView == 43 ||
        debugView == 44 ||
        debugView == 78;
    r.wantsSpecAtrousOutputDebug =
        debugView == 60 ||
        debugView == 73 ||
        debugView == 93 ||
        debugView == 94 ||
        debugView == 95;

    r.wantsHistorySelectDebug = IsHistorySelectDebug(debugView);

    r.wantsSplitDebug = IsSplitDebug(debugView);
    r.wantsMotionDebug = IsMotionDebug(debugView);
    r.wantsMotionDilateDebug = IsMotionDilateDebug(debugView);
    r.wantsViewZDebug = IsViewZDebug(debugView);
    r.wantsViewZReconstructDebug = IsViewZReconstructDebug(debugView);
    r.wantsSurfaceIdDebug = IsSurfaceIdDebug(debugView);
    r.wantsDiffuseAlbedoDebug = IsDiffuseAlbedoDebug(debugView);
    r.wantsDiffuseDemodDebug = IsDiffuseDemodulateDebug(debugView);
    r.wantsOutlierClampDebug = IsOutlierClampDebug(debugView);

    r.wantsSpatialDebug = r.wantsSvgfDebug;

    r.wantsRtPostDebug =
        r.wantsTemporalDebug ||
        r.wantsSvgfDebug ||
        r.wantsHistorySelectDebug;

    r.wantsProducerDebug =
        r.wantsSplitDebug ||
        r.wantsMotionDebug ||
        r.wantsMotionDilateDebug ||
        r.wantsViewZDebug ||
        r.wantsViewZReconstructDebug ||
        r.wantsSurfaceIdDebug ||
        r.wantsDiffuseAlbedoDebug ||
        r.wantsDiffuseDemodDebug ||
        r.wantsOutlierClampDebug;

    r.wantsRtInspectionDebug =
        r.wantsRtPostDebug ||
        r.wantsProducerDebug;

    if (r.wantsSplitDebug ||
        r.wantsMotionDebug ||
        r.wantsViewZDebug ||
        r.wantsSurfaceIdDebug ||
        r.wantsDiffuseAlbedoDebug)
    {
        r.owner = RtDebugOwner::RayGen;
    }

    else if (r.wantsMotionDilateDebug ||
        r.wantsViewZReconstructDebug ||
        r.wantsDiffuseDemodDebug ||
        r.wantsOutlierClampDebug)
    {
        r.owner = RtDebugOwner::GuideReconstruct;
    }
    else if (r.wantsTemporalDebug)
    {
        r.owner = RtDebugOwner::Temporal;
    }
    else if (r.wantsHistorySelectDebug)
    {
        r.owner = RtDebugOwner::HistorySelect;
    }
    else if (r.wantsSvgfDebug)
    {
        r.owner = RtDebugOwner::Spatial;
    }
    else
    {
        r.owner = RtDebugOwner::None;
    }

    return r;
}

Renderer::RtPostMode Renderer::ResolveRtPostMode(const RtDebugRouting& debug) const
{
    const bool enableRtPostStack =
        m_rtEnablePostStack &&
        !debug.DisablesPostStack() &&
        (m_debugView == 0 || debug.wantsRtPostDebug);

    RtPostMode rtPostMode =
        enableRtPostStack ? m_rtPostMode : RtPostMode::Disabled;

    // 60 is spec A-Trous-owned output diagnostic.
    // Stop at spec A-Trous so final combine cannot overwrite it.
    if (debug.wantsSpecAtrousOutputDebug &&
        rtPostMode != RtPostMode::Disabled)
    {
        rtPostMode = RtPostMode::SpecAtrousOnly;
    }
    // 43/44 are diffuse A-Trous-owned output diagnostics.
    // Stop at diffuse A-Trous so final combine cannot overwrite them.
    else if (debug.wantsAtrousOutputDebug &&
        rtPostMode != RtPostMode::Disabled)
    {
        rtPostMode = RtPostMode::DiffuseAtrousOnly;
    }

    return rtPostMode;
}

Renderer::RtDenoiserGuides Renderer::BuildRtDenoiserGuides() const
{
    RtDenoiserGuides g{};

    if (m_rtAovReady)
    {
        g.normalRough = m_rtAovNormal.Get();
        g.depth = m_rtAovDepth.Get();
    }

    if (m_rtAovMotionReady)
        g.prevUVRaw = m_rtAovMotion.Get();

    if (m_rtAovMotionDilatedReady)
        g.prevUVDilated = m_rtAovMotionDilated.Get();

    if (m_rtAovMotionConfReady)
        g.motionConf = m_rtAovMotionConf.Get();

    if (m_rtAovViewZRawReady)
        g.viewZRaw = m_rtAovViewZRaw.Get();

    if (m_rtAovViewZReconsReady)
        g.viewZRecons = m_rtAovViewZRecons.Get();

    if (m_rtAovViewZReconsConfReady)
        g.viewZReconsConf = m_rtAovViewZReconsConf.Get();

    if (m_rtAovSurfaceIdReady)
        g.surfaceId = m_rtAovSurfaceId.Get();

    if (m_rtAovDiffuseAlbedoReady)
        g.diffuseAlbedo = m_rtAovDiffuseAlbedo.Get();

    if (m_rtDiffuseDemodulatedReady)
        g.diffuseDemodulated = m_rtDiffuseDemodulated.Get();

    if (m_rtViewZHistoryValid &&
        m_rtHistoryViewZ[m_rtHistoryReadIndex] &&
        m_rtHistoryViewZConf[m_rtHistoryReadIndex])
    {
        g.viewZHistoryRead = m_rtHistoryViewZ[m_rtHistoryReadIndex].Get();
        g.viewZConfHistoryRead = m_rtHistoryViewZConf[m_rtHistoryReadIndex].Get();
    }

    return g;
}

Renderer::RtDenoiserHistories Renderer::BuildRtDenoiserHistories(
    uint32_t readIndex,
    uint32_t writeIndex) const
{
    RtDenoiserHistories h{};

    h.diffuseRead.signal = m_rtHistoryAccum[readIndex].Get();
    h.diffuseRead.moments = m_rtHistoryMoments[readIndex].Get();

    h.diffuseWrite.signal = m_rtHistoryAccum[writeIndex].Get();
    h.diffuseWrite.moments = m_rtHistoryMoments[writeIndex].Get();

    h.specStableRead.signal = m_rtHistorySpec[readIndex].Get();
    h.specStableRead.moments = m_rtHistoryMomentsSpec[readIndex].Get();

    h.specStableWrite.signal = m_rtHistorySpec[writeIndex].Get();
    h.specStableWrite.moments = m_rtHistoryMomentsSpec[writeIndex].Get();

    h.specResponsiveRead.signal = m_rtHistorySpecResp[readIndex].Get();
    h.specResponsiveRead.moments = m_rtHistoryMomentsSpecResp[readIndex].Get();

    h.specResponsiveWrite.signal = m_rtHistorySpecResp[writeIndex].Get();
    h.specResponsiveWrite.moments = m_rtHistoryMomentsSpecResp[writeIndex].Get();

    h.normalRead = m_rtHistoryNormal[readIndex].Get();
    h.normalWrite = m_rtHistoryNormal[writeIndex].Get();

    h.depthRead = m_rtHistoryDepth[readIndex].Get();
    h.depthWrite = m_rtHistoryDepth[writeIndex].Get();

    h.viewZRead = m_rtHistoryViewZ[readIndex].Get();
    h.viewZWrite = m_rtHistoryViewZ[writeIndex].Get();

    h.viewZConfRead = m_rtHistoryViewZConf[readIndex].Get();
    h.viewZConfWrite = m_rtHistoryViewZConf[writeIndex].Get();

    h.surfaceIdRead = m_rtHistorySurfaceId[readIndex].Get();
    h.surfaceIdWrite = m_rtHistorySurfaceId[writeIndex].Get();

    return h;
}

DescriptorAllocator::Allocation& Renderer::EnsureRtDescriptorTable(
    DescriptorAllocator::Allocation& table,
    uint32_t& currentCount,
    uint32_t expectedCount)
{
    if (!table.IsValid() ||
        currentCount != expectedCount)
    {
        table = m_srvHeap.Allocate(expectedCount);
        currentCount = expectedCount;
    }

    return table;
}

void Renderer::RunRtDenoiser(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height)
{
    auto* cmdList = cl.Get();

    const RtDebugRouting rtDebug =
        BuildRtDebugRouting(m_debugView);

    const RtPostMode rtPostMode =
        ResolveRtPostMode(rtDebug);

    const bool diffuseIsDemodulated =
        rtPostMode != RtPostMode::RawCombineOnly;

    const bool wantsSvgfDebug = rtDebug.wantsSvgfDebug;
    const bool wantsAtrousOutputDebug = rtDebug.wantsAtrousOutputDebug;
    const bool wantsSpecAtrousOutputDebug = rtDebug.wantsSpecAtrousOutputDebug;
    const bool wantsHistorySelectDebug = rtDebug.wantsHistorySelectDebug;

    bool ranDiffuseTemporal = false;
    bool ranSpecStableTemporal = false;
    bool ranSpecResponsiveTemporal = false;
    bool ranHistorySelect = false;

    RtDenoiserGuides guides =
        BuildRtDenoiserGuides();

    RtDenoiserSignals rtSignals{};

    const uint32_t writeIndex =
        1u - m_rtHistoryReadIndex;


    if (rtPostMode == RtPostMode::RawCombineOnly)
    {
        RunRtRawCombineOnly(
            device,
            cl,
            frameIndex,
            width,
            height);

        return;
    }

    // ---------------------------------------------------------------------
    // GuideReconstructionPath
    // Existing stages, same order:
    //   1. ViewZReconstruct
    //   2. MotionDilate
    // ---------------------------------------------------------------------

    const bool temporalWouldRun =
        RtPostModeRunsTemporal(rtPostMode) &&
        m_rtTemporal &&
        (
            m_debugView == 0 ||
            rtDebug.wantsTemporalDebug ||
            rtDebug.wantsHistorySelectDebug ||
            rtDebug.wantsSvgfDebug
        ) &&
        guides.ReadyForTemporal() &&
        guides.ReadyForMotionDilate() &&
        guides.ReadyForViewZReconstruct() &&
        guides.surfaceId;

    const bool postMayRunSpecSpatial =
        RtPostModeRunsSpecAtrous(rtPostMode) &&
        m_rtSvgf &&
        m_rtViewZSigmaScale > 0.0f;

    const bool shouldRunViewZReconstruct =
        temporalWouldRun ||
        postMayRunSpecSpatial ||
        rtDebug.wantsMotionDilateDebug ||
        rtDebug.wantsViewZReconstructDebug ||
        rtDebug.wantsOutlierClampDebug;

    const bool ranViewZReconstruct =
        shouldRunViewZReconstruct &&
        RunRtViewZReconstruct(
            cl,
            frameIndex,
            device,
            width,
            height);

    if (ranViewZReconstruct)
    {
        guides.viewZRecons = m_rtAovViewZRecons.Get();
        guides.viewZReconsConf = m_rtAovViewZReconsConf.Get();
    }

    const bool ranMotionDilate =
        (temporalWouldRun ||
            rtDebug.wantsMotionDilateDebug ||
            rtDebug.wantsOutlierClampDebug) &&
        RunRtMotionDilate(
            cl,
            frameIndex,
            device,
            width,
            height,
            ranViewZReconstruct);

    const bool specTemporalViewZAvailable =
        m_rtViewZSigmaScale > 0.0f &&
        ranViewZReconstruct &&
        m_rtViewZHistoryValid &&
        guides.viewZRecons &&
        guides.viewZHistoryRead &&
        guides.viewZConfHistoryRead;

    const float specTemporalViewZSigmaScale =
        specTemporalViewZAvailable ? m_rtViewZSigmaScale : 0.0f;

    const bool specSpatialViewZAvailable =
        m_rtViewZSigmaScale > 0.0f &&
        ranViewZReconstruct &&
        guides.viewZRecons &&
        guides.viewZReconsConf;

    const float specSpatialViewZSigmaScale =
        specSpatialViewZAvailable ? m_rtViewZSigmaScale : 0.0f;

    if (ranMotionDilate)
    {
        guides.prevUVDilated = m_rtAovMotionDilated.Get();
        guides.motionConf = m_rtAovMotionConf.Get();
    }

    const bool outlierClampCanUseViewZ =
        ranViewZReconstruct &&
        guides.viewZRecons &&
        guides.viewZReconsConf;

    const bool outlierClampCanUseMotionConf =
        ranMotionDilate &&
        guides.motionConf;

    // ---------------------------------------------------------------------
    // SignalPrepPath
    // Existing stage:
    //   DiffuseDemodulate
    // ---------------------------------------------------------------------

    const bool postNeedsDemodulatedDiffuse =
        rtPostMode != RtPostMode::Disabled &&
        rtPostMode != RtPostMode::RawCombineOnly;

    const bool wantsDiffuseOutlierDebug =
        m_debugView == 81 ||
        m_debugView == 83 ||
        m_debugView == 86;

    const bool wantsSpecOutlierDebug =
        m_debugView == 82;

    const bool shouldRunDiffuseDemodulate =
        postNeedsDemodulatedDiffuse ||
        rtDebug.wantsDiffuseDemodDebug ||
        wantsDiffuseOutlierDebug;

    const bool ranDiffuseDemodulate =
        shouldRunDiffuseDemodulate &&
        RunRtDiffuseDemodulate(
            cl,
            frameIndex,
            device,
            width,
            height);

    if (ranDiffuseDemodulate)
    {
        guides.diffuseDemodulated = m_rtDiffuseDemodulated.Get();
    }

    ID3D12Resource* diffuseTemporalInput =
        ranDiffuseDemodulate
        ? guides.diffuseDemodulated
        : m_rtAccumDiffuse.Get();

    ID3D12Resource* specTemporalInput =
        m_rtAccumSpec.Get();

    const bool shouldClampDiffuse =
        m_rtEnableOutlierClamp &&
        m_rtDiffuseRobustInputReady &&
        diffuseTemporalInput &&
        (
            temporalWouldRun ||
            RtPostModeRunsDiffuseAtrous(rtPostMode) ||
            wantsDiffuseOutlierDebug
            );

    const bool ranDiffuseClamp =
        shouldClampDiffuse &&
        RunRtOutlierClamp(
            cl,
            frameIndex,
            device,
            width,
            height,
            false, // diffuse
            diffuseTemporalInput,
            m_rtDiffuseRobustInput.Get(),
            outlierClampCanUseViewZ,
            outlierClampCanUseMotionConf,
            wantsDiffuseOutlierDebug);

    if (ranDiffuseClamp)
    {
        diffuseTemporalInput = m_rtDiffuseRobustInput.Get();
    }

    const bool shouldClampSpec =
        m_rtEnableOutlierClamp &&
        m_rtSpecRobustInputReady &&
        specTemporalInput &&
        (
            temporalWouldRun ||
            RtPostModeRunsSpecAtrous(rtPostMode) ||
            wantsSpecOutlierDebug
            );

    const bool ranSpecClamp =
        shouldClampSpec &&
        RunRtOutlierClamp(
            cl,
            frameIndex,
            device,
            width,
            height,
            true, // specular
            specTemporalInput,
            m_rtSpecRobustInput.Get(),
            outlierClampCanUseViewZ,
            outlierClampCanUseMotionConf,
            wantsSpecOutlierDebug);

    if (ranSpecClamp)
    {
        specTemporalInput = m_rtSpecRobustInput.Get();
    }

    // Fallback signal setup, now using robust inputs when available.
    rtSignals.diffuse =
    {
        diffuseTemporalInput,
        m_rtHistoryMoments[m_rtHistoryReadIndex].Get()
    };

    rtSignals.specStable =
    {
        specTemporalInput,
        m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get()
    };

    rtSignals.specResponsive =
    {
        specTemporalInput,
        m_rtHistoryMomentsSpecResp[m_rtHistoryReadIndex].Get()
    };

    rtSignals.specSelected =
    {
        specTemporalInput,
        rtSignals.specStable.moments
    };

    if (temporalWouldRun &&
        ranMotionDilate &&
        ranDiffuseDemodulate)
    {
        CmdBeginEvent(cmdList, "RT Temporal");

        cl.Transition(diffuseTemporalInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(specTemporalInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtAovMotionDilated.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.Transition(m_rtHistoryAccum[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryMoments[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryNormal[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryDepth[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl.Transition(m_rtHistoryAccum[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtHistoryMoments[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cl.Transition(m_rtHistorySpec[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistorySpec[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtHistoryMomentsSpec[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cl.Transition(m_rtHistorySpecResp[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryMomentsSpecResp[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistorySpecResp[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtHistoryMomentsSpecResp[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(guides.surfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (m_rtSurfaceIdHistoryValid)
        {
            cl.Transition(
                m_rtHistorySurfaceId[m_rtHistoryReadIndex].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (specTemporalViewZAvailable)
        {
            cl.Transition(guides.viewZRecons, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(guides.viewZHistoryRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(guides.viewZConfHistoryRead, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        cl.FlushBarriers();

        // Diffuse temporal
        const bool okDiffuse = UpdateRtTemporalTables(
            frameIndex,
            device,
            diffuseTemporalInput,
            m_rtHistoryAccum[m_rtHistoryReadIndex].Get(),
            m_rtHistoryMoments[m_rtHistoryReadIndex].Get(),
            m_rtHistoryAccum[writeIndex].Get(),
            m_rtHistoryMoments[writeIndex].Get(),
            nullptr,
            nullptr,
            nullptr,
            guides.surfaceId,
            m_rtSurfaceIdHistoryValid
            ? m_rtHistorySurfaceId[m_rtHistoryReadIndex].Get()
            : nullptr,
            m_rtFrames[frameIndex].temporalDiffuseSrvTable,
            m_rtFrames[frameIndex].temporalDiffuseSrvCount,
            m_rtFrames[frameIndex].temporalDiffuseUavTable,
            m_rtFrames[frameIndex].temporalDiffuseUavCount);

        if (okDiffuse)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS cb =
                UpdateRtTemporalConstants(
                    frameIndex,
                    width,
                    height,
                    m_rtTemporalAlpha,
                    m_rtTemporalRoughnessSigma,
                    m_rtTemporalMotionConfMinDiffuse,
                    m_rtTemporalMotionConfPowerDiffuse,
                    0.0f,
                    m_rtSurfaceIdHistoryValid,
                    m_rtTemporalHistoryClampStrengthDiffuse,
                    m_rtSignalDeltaSigmaDiffuse,
                    m_rtConfidencePowerDiffuse,
                    m_rtAntiLagStrengthDiffuse);

            m_rtTemporalPass.Dispatch(
                cl,
                cb,
                m_rtFrames[frameIndex].temporalDiffuseSrvTable.gpu,
                m_rtFrames[frameIndex].temporalDiffuseUavTable.gpu,
                width,
                height);

            D3D12_RESOURCE_BARRIER barriers[3]{};

            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = m_rtHistoryAccum[writeIndex].Get();

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[1].UAV.pResource = m_rtOutput.Get();

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[2].UAV.pResource = m_rtHistoryMoments[writeIndex].Get();

            cmdList->ResourceBarrier(3, barriers);

            rtSignals.diffuse = 
            {
                m_rtHistoryAccum[writeIndex].Get(),
                m_rtHistoryMoments[writeIndex].Get()
            };

            ranDiffuseTemporal = true;
        }
        // Spec stable temporal
        const bool okSpecStable = UpdateRtTemporalTables(
            frameIndex,
            device,
            specTemporalInput,
            m_rtHistorySpec[m_rtHistoryReadIndex].Get(),
            m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get(),
            m_rtHistorySpec[writeIndex].Get(),
            m_rtHistoryMomentsSpec[writeIndex].Get(),
            specTemporalViewZAvailable ? guides.viewZRecons : nullptr,
            specTemporalViewZAvailable ? guides.viewZHistoryRead : nullptr,
            specTemporalViewZAvailable ? guides.viewZConfHistoryRead : nullptr,
            guides.surfaceId,
            m_rtSurfaceIdHistoryValid
            ? m_rtHistorySurfaceId[m_rtHistoryReadIndex].Get()
            : nullptr,
            m_rtFrames[frameIndex].temporalSpecStableSrvTable,
            m_rtFrames[frameIndex].temporalSpecStableSrvCount,
            m_rtFrames[frameIndex].temporalSpecStableUavTable,
            m_rtFrames[frameIndex].temporalSpecStableUavCount);

        if (okSpecStable)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS cb =
                UpdateRtTemporalConstants(
                    frameIndex,
                    width,
                    height,
                    m_rtTemporalAlpha,
                    m_rtTemporalRoughnessSigma,
                    m_rtTemporalMotionConfMinSpec,
                    m_rtTemporalMotionConfPowerSpec,
                    specTemporalViewZSigmaScale,
                    m_rtSurfaceIdHistoryValid,
                    m_rtTemporalHistoryClampStrengthSpec,
                    m_rtSignalDeltaSigmaSpec,
                    m_rtConfidencePowerSpec,
                    m_rtAntiLagStrengthSpec);

            m_rtTemporalPass.Dispatch(
                cl,
                cb,
                m_rtFrames[frameIndex].temporalSpecStableSrvTable.gpu,
                m_rtFrames[frameIndex].temporalSpecStableUavTable.gpu,
                width,
                height);

            D3D12_RESOURCE_BARRIER barriers[3]{};

            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = m_rtHistorySpec[writeIndex].Get();

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[1].UAV.pResource = m_rtOutput.Get();

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[2].UAV.pResource = m_rtHistoryMomentsSpec[writeIndex].Get();

            cmdList->ResourceBarrier(3, barriers);

            rtSignals.specStable = 
            {
                m_rtHistorySpec[writeIndex].Get(),
                m_rtHistoryMomentsSpec[writeIndex].Get()
            };

            rtSignals.specSelected = rtSignals.specStable;
            ranSpecStableTemporal = true;
        }
        // Spec responsive temporal
        const bool okSpecResp = UpdateRtTemporalTables(
            frameIndex,
            device,
            specTemporalInput,
            m_rtHistorySpecResp[m_rtHistoryReadIndex].Get(),
            m_rtHistoryMomentsSpecResp[m_rtHistoryReadIndex].Get(),
            m_rtHistorySpecResp[writeIndex].Get(),
            m_rtHistoryMomentsSpecResp[writeIndex].Get(),
            specTemporalViewZAvailable ? guides.viewZRecons : nullptr,
            specTemporalViewZAvailable ? guides.viewZHistoryRead : nullptr,
            specTemporalViewZAvailable ? guides.viewZConfHistoryRead : nullptr,
            guides.surfaceId,
            m_rtSurfaceIdHistoryValid
            ? m_rtHistorySurfaceId[m_rtHistoryReadIndex].Get()
            : nullptr,
            m_rtFrames[frameIndex].temporalSpecRespSrvTable,
            m_rtFrames[frameIndex].temporalSpecRespSrvCount,
            m_rtFrames[frameIndex].temporalSpecRespUavTable,
            m_rtFrames[frameIndex].temporalSpecRespUavCount);

        if (okSpecResp)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS cb =
                UpdateRtTemporalConstants(
                    frameIndex,
                    width,
                    height,
                    m_rtTemporalAlphaResp,
                    m_rtTemporalRoughnessSigmaResp,
                    m_rtTemporalMotionConfMinSpec,
                    m_rtTemporalMotionConfPowerSpec,
                    specTemporalViewZSigmaScale,
                    m_rtSurfaceIdHistoryValid,
                    m_rtTemporalHistoryClampStrengthSpec,
                    m_rtSignalDeltaSigmaSpec,
                    m_rtConfidencePowerSpec,
                    m_rtAntiLagStrengthSpec);

            m_rtTemporalPass.Dispatch(
                cl,
                cb,
                m_rtFrames[frameIndex].temporalSpecRespSrvTable.gpu,
                m_rtFrames[frameIndex].temporalSpecRespUavTable.gpu,
                width,
                height);

            D3D12_RESOURCE_BARRIER barriers[3]{};

            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = m_rtHistorySpecResp[writeIndex].Get();

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[1].UAV.pResource = m_rtOutput.Get();

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[2].UAV.pResource = m_rtHistoryMomentsSpecResp[writeIndex].Get();

            cmdList->ResourceBarrier(3, barriers);

            rtSignals.specResponsive = 
            {
                m_rtHistorySpecResp[writeIndex].Get(),
                m_rtHistoryMomentsSpecResp[writeIndex].Get()
            };
            ranSpecResponsiveTemporal = true;
        }

        CmdEndEvent(cmdList);
    }

    const bool temporalOnlyReady =
        ranDiffuseTemporal &&
        ranSpecStableTemporal;

    const bool advancedSplitHistory =
        ranDiffuseTemporal &&
        ranSpecStableTemporal &&
        ranSpecResponsiveTemporal;

    if (rtPostMode == RtPostMode::TemporalOnly &&
        temporalOnlyReady)
    {
        RunRtTemporalOnlyCombine(
            device,
            cl,
            frameIndex,
            rtSignals.diffuse.signal,
            rtSignals.specStable.signal,
            width,
            height);
    }

    if (RtPostModeRunsHistorySelect(rtPostMode) &&
        advancedSplitHistory &&
        (m_debugView == 0 || wantsHistorySelectDebug) &&
        m_rtAovReady && m_rtAovMotionConfReady &&
        m_rtSpecSelectedMomentsReady)
    {
        CmdBeginEvent(cmdList, "RT Spec History Select");

        cl.Transition(rtSignals.specStable.signal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(rtSignals.specResponsive.signal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(rtSignals.specStable.moments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(rtSignals.specResponsive.moments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(guides.motionConf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtSvgfPing[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtSpecSelectedMoments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(guides.surfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (m_rtSurfaceIdHistoryValid)
        {
            cl.Transition(
                m_rtHistorySurfaceId[m_rtHistoryReadIndex].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (specSpatialViewZAvailable)
        {
            cl.Transition(guides.viewZRecons, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(guides.viewZReconsConf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        cl.FlushBarriers();

        const bool ok = UpdateRtHistorySelectTables(
            frameIndex,
            device,
            rtSignals.specStable.signal,
            rtSignals.specResponsive.signal,
            rtSignals.specStable.moments,
            rtSignals.specResponsive.moments);

        if (ok)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS cb =
                UpdateRtHistorySelectConstants(frameIndex);

            m_rtHistorySelectPass.Dispatch(
                cl,
                cb,
                m_rtFrames[frameIndex].historySelectSrvTable.gpu,
                m_rtFrames[frameIndex].historySelectUavTable.gpu,
                width,
                height);

            D3D12_RESOURCE_BARRIER barriers[3]{};

            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = m_rtSvgfPing[0].Get();

            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[1].UAV.pResource = m_rtOutput.Get();

            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[2].UAV.pResource = m_rtSpecSelectedMoments.Get();

            cmdList->ResourceBarrier(3, barriers);

            rtSignals.specSelected = 
            {
                m_rtSvgfPing[0].Get(),
                m_rtSpecSelectedMoments.Get()
            };

            ranHistorySelect = true;
        }

        CmdEndEvent(cmdList);
    }

    if (rtPostMode == RtPostMode::HistorySelectOnly &&
        ranHistorySelect)
    {
        RunRtHistorySelectOnlyCombine(
            device,
            cl,
            frameIndex,
            rtSignals.diffuse.signal,
            rtSignals.specSelected.signal,
            width,
            height);
    }

    const bool wantsSpatialDebug =
        m_rtSvgf && wantsSvgfDebug;

    // A-Trous and denoise fallback both consume motion confidence.
    const bool spatialStageNeedsMotionConf =
        RtPostModeRunsAnyAtrous(rtPostMode) &&
        (m_rtSvgf || m_rtDenoise);

    const bool spatialStageAllowed =
        RtPostModeRunsAnyAtrous(rtPostMode) &&
        (m_debugView == 0 || wantsSpatialDebug) &&
        (m_rtAccumulateThisFrame || wantsSpatialDebug) &&
        guides.ReadyForSpatial(spatialStageNeedsMotionConf) &&
        m_rtPostReady &&
        (m_rtSvgf || m_rtDenoise);

    if (spatialStageAllowed)
    {
        if (RtPostModeRunsSpecAtrous(rtPostMode))
        {
            // ---------------------------------------------------------------------
            // Spec A-Trous
            // ---------------------------------------------------------------------
            if (m_rtSvgf)
            {
                CmdBeginEvent(cmdList, "RT A-Trous Spec");

                const uint32_t iterCount =
                    std::min(m_rtAtrousIterationsSpec, kMaxRtAtrousIterations);

                ID3D12Resource* svgfSignal =
                    rtSignals.specSelected.signal;

                ID3D12Resource* svgfMoments =
                    rtSignals.specSelected.moments;

                for (uint32_t iter = 0; iter < iterCount; ++iter)
                {
                    const bool finalIter = (iter + 1 == iterCount);
                   
                    const uint32_t pingIndex = ranHistorySelect
                        ? ((iter + 1u) & 1u)
                        : (iter & 1u);

                    ID3D12Resource* outRes = finalIter
                        ? (wantsSpecAtrousOutputDebug ? m_rtOutput.Get() : m_rtPostSpec.Get())
                        : m_rtSvgfPing[pingIndex].Get();

                    cl.Transition(svgfSignal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.motionConf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(svgfMoments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(outRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    if (specSpatialViewZAvailable)
                    {
                        cl.Transition(
                            guides.viewZRecons,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                        cl.Transition(
                            guides.viewZReconsConf,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }
                    cl.FlushBarriers();

                    const bool ok = UpdateRtSvgfSrvTable(
                        frameIndex,
                        iter,
                        device,
                        m_rtFrames[frameIndex].svgfSpecSrvTables[iter],
                        m_rtFrames[frameIndex].svgfSpecSrvCounts[iter],
                        svgfSignal,
                        svgfMoments,
                        guides.motionConf,
                        specSpatialViewZAvailable ? guides.viewZRecons : nullptr,
                        specSpatialViewZAvailable ? guides.viewZReconsConf : nullptr,
                        guides.surfaceId);

                    if (ok)
                    {
                        const D3D12_GPU_VIRTUAL_ADDRESS cb =
                            UpdateRtAtrousConstants(
                                frameIndex,
                                width,
                                height,
                                iter,
                                advancedSplitHistory,
                                false,
                                m_rtTemporalMotionConfPowerSpec,
                                m_rtTemporalMotionConfMinSpec,
                                specSpatialViewZSigmaScale);

                        D3D12_GPU_DESCRIPTOR_HANDLE outUav = finalIter
                            ? (wantsSpecAtrousOutputDebug ? m_rtOutputUav.gpu : RtPostUavGpuAt(1))
                            : RtSvgfPingUavGpuAt(pingIndex);

                        m_rtAtrousPass.Dispatch(
                            cl,
                            cb,
                            m_rtFrames[frameIndex].svgfSpecSrvTables[iter].gpu,
                            outUav,
                            width,
                            height);

                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = outRes;
                        cmdList->ResourceBarrier(1, &b);

                        if (!finalIter)
                        {
                            svgfSignal = outRes;
                        }
                        else if (!wantsSpecAtrousOutputDebug)
                        {
                            rtSignals.specFinal = 
                            {
                                m_rtPostSpec.Get(),
                                rtSignals.specSelected.moments
                            };
                        }
                    }
                }

                CmdEndEvent(cmdList);
            }
            else if (m_rtDenoise)
            {
                // ---------------------------------------------------------------------
                // Spec denoise fallback
                // ---------------------------------------------------------------------

                const bool ok =
                    RunRtDenoiseSignal(
                        cl,
                        frameIndex,
                        device,
                        "RT Denoise Spec",
                        rtSignals.specSelected.signal,
                        m_rtFrames[frameIndex].denoiseSpecSrvTable,
                        m_rtFrames[frameIndex].denoiseSpecSrvCount,
                        m_rtPostSpec.Get(),
                        RtPostUavGpuAt(1),
                        width,
                        height,
                        m_rtTemporalMotionConfMinSpec,
                        m_rtTemporalMotionConfPowerSpec,
                        guides.surfaceId);

                if (ok)
                {
                    rtSignals.specFinal =
                    {
                        m_rtPostSpec.Get(),
                        rtSignals.specSelected.moments
                    };
                }
            }
        }

        if (RtPostModeRunsDiffuseAtrous(rtPostMode))
        {
            // ---------------------------------------------------------------------
            // Diffuse A-Trous
            // ---------------------------------------------------------------------
            if (m_rtSvgf)
            {
                CmdBeginEvent(cmdList, "RT A-Trous Diffuse");

                const uint32_t iterCount =
                    std::min(m_rtAtrousIterations, kMaxRtAtrousIterations);



                ID3D12Resource* svgfSignal =
                    rtSignals.diffuse.signal;

                ID3D12Resource* svgfMoments =
                    rtSignals.diffuse.moments;

                const bool diffuseAtrousDebugToOutput = wantsAtrousOutputDebug;
                for (uint32_t iter = 0; iter < iterCount; ++iter)
                {
                    const bool finalIter = (iter + 1 == iterCount);
                    const uint32_t pingIndex = iter & 1u;

                    ID3D12Resource* outRes = finalIter
                        ? (diffuseAtrousDebugToOutput ? m_rtOutput.Get() : m_rtPostDiffuse.Get())
                        : m_rtSvgfPing[pingIndex].Get();

                    cl.Transition(svgfSignal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(svgfMoments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(outRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cl.Transition(guides.motionConf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.Transition(guides.surfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    cl.FlushBarriers();

                    const bool ok = UpdateRtSvgfSrvTable(
                        frameIndex,
                        iter,
                        device,
                        m_rtFrames[frameIndex].svgfDiffuseSrvTables[iter],
                        m_rtFrames[frameIndex].svgfDiffuseSrvCounts[iter],
                        svgfSignal,
                        svgfMoments,
                        guides.motionConf,
                        nullptr,
                        nullptr,
                        guides.surfaceId);

                    if (ok)
                    {
                        const D3D12_GPU_VIRTUAL_ADDRESS cb =
                            UpdateRtAtrousConstants(
                                frameIndex,
                                width,
                                height,
                                iter,
                                advancedSplitHistory,
                                false,
                                0.0f,   // motionConfPower <= 0 disables confidence modulation
                                0.0f,
                                0.0f);

                        D3D12_GPU_DESCRIPTOR_HANDLE outUav = finalIter
                            ? (diffuseAtrousDebugToOutput ? m_rtOutputUav.gpu : RtPostUavGpuAt(0))
                            : RtSvgfPingUavGpuAt(pingIndex);

                        m_rtAtrousPass.Dispatch(
                            cl,
                            cb,
                            m_rtFrames[frameIndex].svgfDiffuseSrvTables[iter].gpu,
                            outUav,
                            width,
                            height);

                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.UAV.pResource = outRes;
                        cmdList->ResourceBarrier(1, &b);

                        if (!finalIter)
                        {
                            svgfSignal = outRes;
                        }
                        else if (!diffuseAtrousDebugToOutput)
                        {
                            rtSignals.diffuseFinal = 
                            {
                                m_rtPostDiffuse.Get(),
                                rtSignals.diffuse.moments
                            };
                        }
                    }
                }

                CmdEndEvent(cmdList);
            }

            else if (m_rtDenoise)
            {
                // ---------------------------------------------------------------------
                // Diffuse denoise fallback
                // ---------------------------------------------------------------------

                const bool ok =
                    RunRtDenoiseSignal(
                        cl,
                        frameIndex,
                        device,
                        "RT Denoise Diffuse",
                        rtSignals.diffuse.signal,
                        m_rtFrames[frameIndex].denoiseDiffuseSrvTable,
                        m_rtFrames[frameIndex].denoiseDiffuseSrvCount,
                        m_rtPostDiffuse.Get(),
                        RtPostUavGpuAt(0),
                        width,
                        height,
                        m_rtTemporalMotionConfMinDiffuse,
                        m_rtTemporalMotionConfPowerDiffuse,
                        guides.surfaceId);

                if (ok)
                {
                    rtSignals.diffuseFinal =
                    {
                        m_rtPostDiffuse.Get(),
                        rtSignals.diffuse.moments
                    };
                }
            }
        }
    }

    // Diagnostic/final recomposition.
    // These modes must still produce output even if no spatial filter ran.
    // FinalDiffuseSignal()/FinalSpecSignal() fall back to the best available
    // temporal/selected/raw signals when no A-Trous/denoise output exists.
    //
    // Pass-owned debug views such as temporal debug 18-26 already write their
    // visualization into m_rtOutput. Do not let the final Full combine overwrite
    // them with the normal beauty output.
    const bool allowFullModeFinalCombine =
        m_debugView == 0 ||
        (wantsSvgfDebug && !wantsAtrousOutputDebug);

    if (rtPostMode == RtPostMode::SpecAtrousOnly &&
        !wantsSpecAtrousOutputDebug &&
        rtSignals.diffuse.signal &&
        rtSignals.FinalSpecSignal())
    {
        CombineRtSignalsToOutput(
            device,
            cl,
            frameIndex,
            rtSignals.diffuse.signal,
            rtSignals.FinalSpecSignal(),
            width,
            height,
            "RT Spec Spatial Only Combine",
            diffuseIsDemodulated);
    }
    else if (rtPostMode == RtPostMode::DiffuseAtrousOnly &&
        !wantsAtrousOutputDebug &&
        rtSignals.FinalDiffuseSignal() &&
        rtSignals.specSelected.signal)
    {
        CombineRtSignalsToOutput(
            device,
            cl,
            frameIndex,
            rtSignals.FinalDiffuseSignal(),
            rtSignals.specSelected.signal,
            width,
            height,
            "RT Diffuse Spatial Only Combine",
            diffuseIsDemodulated);
    }
    
    if (rtPostMode == RtPostMode::Full &&
        allowFullModeFinalCombine &&
        rtSignals.FinalDiffuseSignal() &&
        rtSignals.FinalSpecSignal())
    {
        CombineRtSignalsToOutput(
            device,
            cl,
            frameIndex,
            rtSignals.FinalDiffuseSignal(),
            rtSignals.FinalSpecSignal(),
            width,
            height,
            "RT Combine",
            diffuseIsDemodulated);
    }

    if (RtPostModeCommitsHistory(rtPostMode) &&
        advancedSplitHistory)
    {
        CmdBeginEvent(cmdList, "RT History Commit");

        const uint32_t writeIndex = 1u - m_rtHistoryReadIndex;

        cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl.Transition(guides.depth, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl.Transition(m_rtHistoryNormal[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        cl.Transition(m_rtHistoryDepth[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        cl.FlushBarriers();

        cl.Get()->CopyResource(m_rtHistoryNormal[writeIndex].Get(), guides.normalRough);
        cl.Get()->CopyResource(m_rtHistoryDepth[writeIndex].Get(), guides.depth);

        if (ranViewZReconstruct && ranMotionDilate && temporalWouldRun)
        {
            CommitRtViewZHistory(cl, writeIndex);
        }
        else if (!temporalWouldRun)
        {
            m_rtViewZHistoryValid = false;
        }

        cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(guides.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl.Transition(m_rtHistoryNormal[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtHistoryDepth[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.FlushBarriers();

        CommitRtSurfaceIdHistory(cl, writeIndex);

        m_rtHistoryReadIndex = writeIndex;
        m_rtTemporalHistoryValid = true;

        CmdEndEvent(cmdList);
    }
}

void Renderer::CommitRtSurfaceIdHistory(
    CommandList& cl,
    uint32_t writeIndex)
{
    if (!m_rtAovSurfaceId ||
        !m_rtAovSurfaceIdReady ||
        !m_rtHistorySurfaceId[writeIndex])
    {
        m_rtSurfaceIdHistoryValid = false;
        return;
    }

    auto* cmdList = cl.Get();

    cl.Transition(
        m_rtAovSurfaceId.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE);

    cl.Transition(
        m_rtHistorySurfaceId[writeIndex].Get(),
        D3D12_RESOURCE_STATE_COPY_DEST);

    cl.FlushBarriers();

    cmdList->CopyResource(
        m_rtHistorySurfaceId[writeIndex].Get(),
        m_rtAovSurfaceId.Get());

    cl.Transition(
        m_rtHistorySurfaceId[writeIndex].Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.FlushBarriers();

    m_rtSurfaceIdHistoryValid = true;
}

bool Renderer::UpdateRtOutlierClampTables(
    uint32_t frameIndex,
    ID3D12Device* device,
    bool specSignal,
    ID3D12Resource* inputResource,
    ID3D12Resource* outputResource,
    bool useViewZ,
    bool useMotionConf,
    bool writeDebug)
{
    if (!device ||
        !inputResource ||
        !outputResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovSurfaceId ||
        !m_rtAovReady ||
        !m_rtAovSurfaceIdReady)
    {
        return false;
    }

    if (writeDebug && (!m_rtOutput || !m_rtOutputReady))
    {
        return false;
    }

    if (useViewZ &&
        (!m_rtAovViewZRecons ||
            !m_rtAovViewZReconsConf ||
            !m_rtAovViewZReconsReady ||
            !m_rtAovViewZReconsConfReady))
    {
        return false;
    }

    if (useMotionConf &&
        (!m_rtAovMotionConf ||
            !m_rtAovMotionConfReady))
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    auto& srvTable = specSignal
        ? frame.outlierSpecSrvTable
        : frame.outlierDiffuseSrvTable;

    auto& uavTable = specSignal
        ? frame.outlierSpecUavTable
        : frame.outlierDiffuseUavTable;

    auto& srvCount = specSignal
        ? frame.outlierSpecSrvCount
        : frame.outlierDiffuseSrvCount;

    auto& uavCount = specSignal
        ? frame.outlierSpecUavCount
        : frame.outlierDiffuseUavCount;

    EnsureRtDescriptorTable(
        srvTable,
        srvCount,
        kRtOutlierClampSrvCount);

    EnsureRtDescriptorTable(
        uavTable,
        uavCount,
        kRtOutlierClampUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = uavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto WriteSrv2D = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    auto WriteUav2D = [&](ID3D12Resource* resource, DXGI_FORMAT format, uint32_t slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            UavAt(slot));
    };

    // t0 = input signal
    WriteSrv2D(inputResource, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);

    // t1 = normal/roughness
    WriteSrv2D(m_rtAovNormal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 1);

    // t2 = depth
    WriteSrv2D(m_rtAovDepth.Get(), DXGI_FORMAT_R32_FLOAT, 2);

    // t3 = surface id
    WriteSrv2D(m_rtAovSurfaceId.Get(), DXGI_FORMAT_R32_UINT, 3);

    // t4 = reconstructed ViewZ, optional null SRV
    WriteSrv2D(
        useViewZ ? m_rtAovViewZRecons.Get() : nullptr,
        DXGI_FORMAT_R16_FLOAT,
        4);

    // t5 = reconstructed ViewZ confidence, optional null SRV
    WriteSrv2D(
        useViewZ ? m_rtAovViewZReconsConf.Get() : nullptr,
        DXGI_FORMAT_R16_FLOAT,
        5);

    // t6 = motion confidence, optional null SRV
    WriteSrv2D(
        useMotionConf ? m_rtAovMotionConf.Get() : nullptr,
        DXGI_FORMAT_R16_FLOAT,
        6);

    // u0 = robust/clamped output
    WriteUav2D(outputResource, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);

    // u1 = debug output when requested; otherwise bind a no-op alias to the
    // robust output so m_rtOutput does not need a UAV transition for non-debug runs.
    if (writeDebug)
    {
        WriteUav2D(m_rtOutput.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    }
    else
    {
        WriteUav2D(outputResource, DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtOutlierClampConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height,
    bool specSignal,
    bool useViewZ,
    bool useMotionConf,
    bool writeDebug)
{
    constexpr uint32_t cbSize =
        (sizeof(RtOutlierClampConstants) + 255u) & ~255u;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<RtOutlierClampConstants*>(alloc.cpu);

    *cb = {};

    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };

    cb->radius = 1;
    cb->signalKind = specSignal ? 1u : 0u;

    cb->depthSigma = m_rtTemporalDepthSigma;
    cb->normalSigma = m_rtTemporalNormalSigma;
    cb->roughnessSigma = 0.20f;
    cb->padGuide0 = 0.0f;

    cb->sigmaK = specSignal
        ? m_rtOutlierClampSpecSigmaK
        : m_rtOutlierClampDiffuseSigmaK;

    const float maxLum =
        std::max(1e-3f, m_rtOutlierClampMaxLuminance);

    cb->maxLuminance = maxLum;
    cb->minNeighborhoodWeight = 1.0f;
    cb->minClampLuminance = 0.0f;

    cb->surfaceIdRequired = 1.0f;
    cb->clampStrength = std::clamp(
        specSignal
        ? m_rtOutlierClampSpecStrength
        : m_rtOutlierClampDiffuseStrength,
        0.0f,
        1.0f);
    cb->motionRelaxation = m_rtTemporalClampRelaxation;
    cb->distanceNormParams = RtDistanceNormParams();
    cb->distanceNormSigma = kRtDistanceNormSigma;
    cb->useViewZ = useViewZ ? 1u : 0u;
    cb->useMotionConf = useMotionConf ? 1u : 0u;
    cb->debugView = writeDebug ? m_debugView : 0u;

    cb->pad0[0] = 0u;
    cb->pad0[1] = 0u;

    return alloc.gpu;
}

bool Renderer::RunRtOutlierClamp(
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
    bool writeDebug)
{
    if (!device ||
        !inputResource ||
        !outputResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovSurfaceId ||
        !m_rtAovReady ||
        !m_rtAovSurfaceIdReady)
    {
        return false;
    }

    if (writeDebug && (!m_rtOutput || !m_rtOutputReady))
    {
        return false;
    }

    if (useViewZ &&
        (!m_rtAovViewZRecons ||
            !m_rtAovViewZReconsConf ||
            !m_rtAovViewZReconsReady ||
            !m_rtAovViewZReconsConfReady))
    {
        return false;
    }

    if (useMotionConf &&
        (!m_rtAovMotionConf ||
            !m_rtAovMotionConfReady))
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(
        cmdList,
        specSignal ? "RT Spec Outlier Clamp" : "RT Diffuse Outlier Clamp");

    cl.Transition(inputResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovSurfaceId.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (useViewZ)
    {
        cl.Transition(m_rtAovViewZRecons.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl.Transition(m_rtAovViewZReconsConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (useMotionConf)
    {
        cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    cl.Transition(outputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (writeDebug)
    {
        cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    cl.FlushBarriers();

    const bool okTables =
        UpdateRtOutlierClampTables(
            frameIndex,
            device,
            specSignal,
            inputResource,
            outputResource,
            useViewZ,
            useMotionConf,
            writeDebug);

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtOutlierClampConstants(
            frameIndex,
            width,
            height,
            specSignal,
            useViewZ,
            useMotionConf,
            writeDebug);

    auto& frame = m_rtFrames[frameIndex];

    const D3D12_GPU_DESCRIPTOR_HANDLE srvTable =
        specSignal
        ? frame.outlierSpecSrvTable.gpu
        : frame.outlierDiffuseSrvTable.gpu;

    const D3D12_GPU_DESCRIPTOR_HANDLE uavTable =
        specSignal
        ? frame.outlierSpecUavTable.gpu
        : frame.outlierDiffuseUavTable.gpu;

    m_rtOutlierClampPass.Dispatch(
        cl,
        cb,
        srvTable,
        uavTable,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    uint32_t barrierCount = 0;

    barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[barrierCount].UAV.pResource = outputResource;
    ++barrierCount;

    if (writeDebug)
    {
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[barrierCount].UAV.pResource = m_rtOutput.Get();
        ++barrierCount;
    }

    cmdList->ResourceBarrier(barrierCount, barriers);

    CmdEndEvent(cmdList);
    return true;
}

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
            dv == 58;
    }

    static bool IsSvgfDebug(uint32_t dv)
    {
        return (dv == 28) ||
            (dv == 43) ||
            (dv == 44) ||
            dv == 60;
    }

    static bool IsHistorySelectDebug(uint32_t dv)
    {
        return
            (dv >= 29 && dv <= 31) ||
            (dv >= 37 && dv <= 42) ||
            dv == 59;
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
    const char* markerName)
{
    if (!m_rtOutputReady || diffuseResource == nullptr || specularResource == nullptr)
        return false;

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, markerName);

    cl.Transition(diffuseResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(specularResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

    m_rtCombinePass.Dispatch(
        cl,
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
        "RT Raw Combine Only");
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
        "RT Temporal Only Combine");
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
        "RT History Select Only Combine");
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
        "RT Spec A-Trous Only Combine");
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
        "RT Diffuse A-Trous Only Combine");
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
        "RT Combine");
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

    const bool wantsTemporalDebug = IsTemporalDebug(m_debugView);
    const bool wantsSvgfDebug = IsSvgfDebug(m_debugView);
    const bool wantsAtrousOutputDebug =
        m_debugView == 43 ||
        m_debugView == 44;
    const bool wantsSpecAtrousOutputDebug =
        m_debugView == 60;
    const bool wantsHistorySelectDebug = IsHistorySelectDebug(m_debugView);
    const bool wantsSplitDebug = IsSplitDebug(m_debugView);
    const bool wantsMotionDebug = IsMotionDebug(m_debugView);
    const bool wantsMotionDilateDebug = IsMotionDilateDebug(m_debugView);
    const bool wantsHitDistDebug = IsHitDistDebug(m_debugView);

    const bool wantsRtPostDebug =
        wantsTemporalDebug ||
        wantsSvgfDebug ||
        wantsHistorySelectDebug;

    const bool wantsHitDistReconstructDebug =
        IsHitDistReconstructDebug(m_debugView);

    const bool wantsRtInspectionDebug =
        wantsRtPostDebug ||
        wantsSplitDebug ||
        wantsMotionDebug ||
        wantsMotionDilateDebug ||
        wantsHitDistDebug ||
        wantsHitDistReconstructDebug;

    // Producer-owned diagnostics write directly to m_rtOutput.
    // Keep the post stack disabled so later temporal/SVGF/combine passes cannot overwrite them.
    const bool wantsProducerDebug =
        wantsSplitDebug ||
        wantsMotionDebug ||
        wantsMotionDilateDebug ||
        wantsHitDistDebug ||
        wantsHitDistReconstructDebug;

    const bool enableRtPostStack =
        m_rtEnablePostStack &&
        !wantsProducerDebug &&
        (m_debugView == 0 || wantsRtPostDebug);

    RtPostMode rtPostMode =
        enableRtPostStack ? m_rtPostMode : RtPostMode::Disabled;

    // A-Trous output diagnostics are post-stack-owned, not producer-owned.
    // Keep the post stack enabled, but stop at the diffuse A-Trous stage so
    // DebugView 43/44 are written by RtAtrousPass and are not recomposed by RtCombine.
    if (wantsSpecAtrousOutputDebug && rtPostMode != RtPostMode::Disabled)
    {
        rtPostMode = RtPostMode::SpecAtrousOnly;
    }
    else if (wantsAtrousOutputDebug && rtPostMode != RtPostMode::Disabled)
    {
        rtPostMode = RtPostMode::DiffuseAtrousOnly;
    }


    const bool allowRtAccumulation =
        m_rtAccumulate &&
        !wantsMotionDilateDebug &&
        ((m_debugView == 0) || wantsRtInspectionDebug) &&
        (
            m_rtTemporal ||
            (!m_autoOrbit && m_pauseAnimation) ||
            wantsSplitDebug ||
            wantsMotionDebug ||
            wantsHitDistDebug
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

    const bool hitDistReconsSettingsChanged =
        std::fabs(m_rtHitDistReconsAlpha - m_prevRtHitDistReconsAlpha) > 1e-6f;

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
        hitDistReconsSettingsChanged ||
        (m_rtTemporalEnableVarianceBoost != m_prevRtTemporalEnableVarianceBoost);

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
        hitDistReconsSettingsChanged)
    {
        m_rtHitDistHistoryValid = false;
    }

    // This must always be assigned before DXR dispatch and before post-stack gating.
    // RayGen receives this through PerFrameConstants::rtAccumulate.
    m_rtAccumulateThisFrame = allowRtAccumulation;

    if (wantsMotionDilateDebug || wantsHitDistReconstructDebug)
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
    m_prevRtHitDistReconsAlpha = m_rtHitDistReconsAlpha;

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

        if (rtPostMode == RtPostMode::RawCombineOnly)
        {
            RunRtRawCombineOnly(
                device,
                cl,
                frameIndex,
                width,
                height);
        }

        const bool temporalWouldRun =
            RtPostModeRunsTemporal(rtPostMode) &&
            m_rtTemporal &&
            (
                m_debugView == 0 ||
                wantsTemporalDebug ||
                wantsHistorySelectDebug ||
                wantsSvgfDebug
            ) &&
            m_rtAovReady &&
            m_rtAovMotionReady &&
            m_rtAovMotionDilatedReady &&
            m_rtAovPrimaryHitDistReady &&
            m_rtAovMotionConfReady;

        const bool shouldRunHitDistReconstruct =
            temporalWouldRun ||
            wantsMotionDilateDebug ||
            wantsHitDistReconstructDebug;

        const bool ranHitDistReconstruct =
            shouldRunHitDistReconstruct &&
            RunRtHitDistReconstruct(
                cl,
                frameIndex,
                device,
                width,
                height);

        const bool ranMotionDilate =
            (temporalWouldRun || wantsMotionDilateDebug) &&
            RunRtMotionDilate(
                cl,
                frameIndex,
                device,
                width,
                height,
                ranHitDistReconstruct);


        //Temporal
        RtPostSignals rtSignals{};

        rtSignals.diffuse.signal = m_rtAccumDiffuse.Get();
        rtSignals.diffuse.moments = m_rtHistoryMoments[m_rtHistoryReadIndex].Get();

        rtSignals.specStable.signal = m_rtAccumSpec.Get();
        rtSignals.specStable.moments = m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get();

        rtSignals.specResponsive.signal = m_rtAccumSpec.Get();
        rtSignals.specResponsive.moments = m_rtHistoryMomentsSpecResp[m_rtHistoryReadIndex].Get();

        rtSignals.specSelected.signal = m_rtAccumSpec.Get();
        rtSignals.specSelected.moments = rtSignals.specStable.moments;

        const uint32_t writeIndex = 1u - m_rtHistoryReadIndex;

        if (temporalWouldRun && ranMotionDilate)
        {
            CmdBeginEvent(cmdList, "RT Temporal");

            cl.Transition(m_rtAccumDiffuse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(m_rtAccumSpec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
            cl.FlushBarriers();
            
            // Diffuse temporal
            const bool okDiffuse = UpdateRtTemporalTables(
                frameIndex,
                device,
                m_rtAccumDiffuse.Get(),
                m_rtHistoryAccum[m_rtHistoryReadIndex].Get(),
                m_rtHistoryMoments[m_rtHistoryReadIndex].Get(),
                m_rtHistoryAccum[writeIndex].Get(),
                m_rtHistoryMoments[writeIndex].Get(),
                m_rtFrames[frameIndex].temporalDiffuseSrvTable,
                m_rtFrames[frameIndex].temporalDiffuseUavTable);
            
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
                        m_rtTemporalMotionConfPowerDiffuse);

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

                rtSignals.diffuse.signal = m_rtHistoryAccum[writeIndex].Get();
                rtSignals.diffuse.moments = m_rtHistoryMoments[writeIndex].Get();
                rtSignals.ranDiffuseTemporal = true;
            }
            // Spec stable temporal
            const bool okSpecStable = UpdateRtTemporalTables(
                frameIndex,
                device,
                m_rtAccumSpec.Get(),
                m_rtHistorySpec[m_rtHistoryReadIndex].Get(),
                m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get(),
                m_rtHistorySpec[writeIndex].Get(),
                m_rtHistoryMomentsSpec[writeIndex].Get(),
                m_rtFrames[frameIndex].temporalSpecStableSrvTable,
                m_rtFrames[frameIndex].temporalSpecStableUavTable);
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
                        m_rtTemporalMotionConfPowerSpec);

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

                rtSignals.specStable.signal = m_rtHistorySpec[writeIndex].Get();
                rtSignals.specStable.moments = m_rtHistoryMomentsSpec[writeIndex].Get();
                rtSignals.ranSpecStableTemporal = true;
                rtSignals.specSelected.signal = rtSignals.specStable.signal;
                rtSignals.specSelected.moments = rtSignals.specStable.moments;
            }
            // Spec responsive temporal
            const bool okSpecResp = UpdateRtTemporalTables(
                frameIndex,
                device,
                m_rtAccumSpec.Get(),
                m_rtHistorySpecResp[m_rtHistoryReadIndex].Get(),
                m_rtHistoryMomentsSpecResp[m_rtHistoryReadIndex].Get(),
                m_rtHistorySpecResp[writeIndex].Get(),
                m_rtHistoryMomentsSpecResp[writeIndex].Get(),
                m_rtFrames[frameIndex].temporalSpecRespSrvTable,
                m_rtFrames[frameIndex].temporalSpecRespUavTable);
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
                        m_rtTemporalMotionConfPowerSpec);

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

                rtSignals.specResponsive.signal = m_rtHistorySpecResp[writeIndex].Get();
                rtSignals.specResponsive.moments = m_rtHistoryMomentsSpecResp[writeIndex].Get();
                rtSignals.ranSpecResponsiveTemporal = true;
            }
           
            CmdEndEvent(cmdList);
        }

        const bool temporalOnlyReady = rtSignals.TemporalReady();
        const bool advancedSplitHistory = rtSignals.AdvancedSplitHistoryReady();

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
            cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(m_rtSvgfPing[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.Transition(m_rtSpecSelectedMoments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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

                rtSignals.specSelected.signal = m_rtSvgfPing[0].Get();
                rtSignals.specSelected.moments = m_rtSpecSelectedMoments.Get(); 
                rtSignals.ranHistorySelect = true;
            }

            CmdEndEvent(cmdList);
        }

        if (rtPostMode == RtPostMode::HistorySelectOnly &&
            rtSignals.ranHistorySelect)
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

        // A-Trous and denoise fallback both consume m_rtAovMotionConf
        const bool spatialStageNeedsMotionConf =
            RtPostModeRunsAnyAtrous(rtPostMode) &&
            (m_rtSvgf || m_rtDenoise);

        const bool spatialStageAllowed =
            RtPostModeRunsAnyAtrous(rtPostMode) &&
            (m_debugView == 0 || wantsSpatialDebug) &&
            (m_rtAccumulateThisFrame || wantsSpatialDebug) &&
            m_rtAovReady &&
            (!spatialStageNeedsMotionConf || m_rtAovMotionConfReady) &&
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

                        ID3D12Resource* svgfSignal = advancedSplitHistory
                            ? rtSignals.specSelected.signal
                            : m_rtAccumSpec.Get();

                        ID3D12Resource* svgfMoments = advancedSplitHistory
                            ? rtSignals.specSelected.moments
                            : m_rtHistoryMomentsSpec[m_rtHistoryReadIndex].Get();

                        for (uint32_t iter = 0; iter < iterCount; ++iter)
                        {
                            const bool finalIter = (iter + 1 == iterCount);

                            const uint32_t pingIndex = rtSignals.ranHistorySelect
                                ? ((iter + 1u) & 1u)
                                : (iter & 1u);

                            ID3D12Resource* outRes = finalIter
                                ? (wantsSpecAtrousOutputDebug ? m_rtOutput.Get() : m_rtPostSpec.Get())
                                : m_rtSvgfPing[pingIndex].Get();

                            cl.Transition(svgfSignal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(svgfMoments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(outRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            cl.FlushBarriers();

                            const bool ok = UpdateRtSvgfSrvTable(
                                frameIndex,
                                iter,
                                device,
                                m_rtFrames[frameIndex].svgfSpecSrvTables[iter],
                                m_rtFrames[frameIndex].svgfSpecSrvCounts[iter],
                                svgfSignal,
                                svgfMoments,
                                m_rtAovMotionConf.Get());

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
                                        m_rtTemporalMotionConfMinSpec);
                                
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
                                    rtSignals.SetFinalSpec(
                                        m_rtPostSpec.Get(),
                                        RtSpatialFilter::Atrous);
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
                                advancedSplitHistory ? rtSignals.specSelected.signal : m_rtAccumSpec.Get(),
                                m_rtFrames[frameIndex].denoiseSpecSrvTable,
                                m_rtFrames[frameIndex].denoiseSpecSrvCount,
                                m_rtPostSpec.Get(),
                                RtPostUavGpuAt(1),
                                width,
                                height,
                                m_rtTemporalMotionConfMinSpec,
                                m_rtTemporalMotionConfPowerSpec);
                        
                        if (ok)
                        {
                            rtSignals.SetFinalSpec(
                                m_rtPostSpec.Get(),
                                RtSpatialFilter::Denoise);
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

                        
                        
                        ID3D12Resource* svgfSignal = advancedSplitHistory
                            ? rtSignals.diffuse.signal
                            : m_rtAccumDiffuse.Get();

                        ID3D12Resource* svgfMoments = advancedSplitHistory
                            ? rtSignals.diffuse.moments
                            : m_rtHistoryMoments[m_rtHistoryReadIndex].Get();
                        const bool diffuseAtrousDebugToOutput = wantsAtrousOutputDebug;
                        for (uint32_t iter = 0; iter < iterCount; ++iter)
                        {
                            const bool finalIter = (iter + 1 == iterCount);
                            const uint32_t pingIndex = iter & 1u;

                            ID3D12Resource* outRes = finalIter
                                ? (diffuseAtrousDebugToOutput ? m_rtOutput.Get() : m_rtPostDiffuse.Get())
                                : m_rtSvgfPing[pingIndex].Get();

                            cl.Transition(svgfSignal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(svgfMoments, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.Transition(outRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            cl.Transition(m_rtAovMotionConf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                            cl.FlushBarriers();

                            const bool ok = UpdateRtSvgfSrvTable(
                                frameIndex,
                                iter,
                                device,
                                m_rtFrames[frameIndex].svgfDiffuseSrvTables[iter],
                                m_rtFrames[frameIndex].svgfDiffuseSrvCounts[iter],
                                svgfSignal,
                                svgfMoments,
                                m_rtAovMotionConf.Get());

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
                                    rtSignals.SetFinalDiffuse(
                                        m_rtPostDiffuse.Get(),
                                        RtSpatialFilter::Atrous);
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
                                advancedSplitHistory ? rtSignals.diffuse.signal : m_rtAccumDiffuse.Get(),
                                m_rtFrames[frameIndex].denoiseDiffuseSrvTable,
                                m_rtFrames[frameIndex].denoiseDiffuseSrvCount,
                                m_rtPostDiffuse.Get(),
                                RtPostUavGpuAt(0),
                                width,
                                height,
                                m_rtTemporalMotionConfMinDiffuse,
                                m_rtTemporalMotionConfPowerDiffuse);

                        if (ok)
                        {
                            rtSignals.SetFinalDiffuse(
                                m_rtPostDiffuse.Get(),
                                RtSpatialFilter::Denoise);
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
                "RT Spec Spatial Only Combine");
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
                "RT Diffuse Spatial Only Combine");
        }
        else if (rtPostMode == RtPostMode::Full &&
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
                "RT Combine");
        }
        
        if (RtPostModeCommitsHistory(rtPostMode) &&
            rtSignals.AdvancedSplitHistoryReady())
        {
            CmdBeginEvent(cmdList, "RT History Commit");

            const uint32_t writeIndex = 1u - m_rtHistoryReadIndex;

            cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl.Transition(m_rtHistoryNormal[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            cl.Transition(m_rtHistoryDepth[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            cl.FlushBarriers();

            cl.Get()->CopyResource(m_rtHistoryNormal[writeIndex].Get(), m_rtAovNormal.Get());
            cl.Get()->CopyResource(m_rtHistoryDepth[writeIndex].Get(), m_rtAovDepth.Get());

            if (ranHitDistReconstruct && ranMotionDilate && temporalWouldRun) 
            {
                CommitRtHitDistHistory(cl, writeIndex);
            }
            else if (!temporalWouldRun)
            {
                m_rtHitDistHistoryValid = false;
            }

            cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl.Transition(m_rtHistoryNormal[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.Transition(m_rtHistoryDepth[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl.FlushBarriers();

            m_rtHistoryReadIndex = writeIndex;
            m_rtTemporalHistoryValid = true;

            CmdEndEvent(cmdList);
        }

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
        cl.Transition(m_rtAovPrimaryHitDist.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (m_rtAovHitDistReconsReady)
        {
            cl.Transition(m_rtAovHitDistRecons.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (m_rtAovHitDistReconsConfReady)
        {
            cl.Transition(m_rtAovHitDistReconsConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
    m_rtAovPrimaryHitDistReady = false;
    m_rtAovHitDistReconsReady = false;
    m_rtAovHitDistReconsConfReady = false;
    m_rtHitDistHistoryValid = false;

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
        m_rtHitDistReconstructPass.Initialize(device, Paths::ShaderDir());
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
        !m_rtAovPrimaryHitDist ||
        !m_rtAovPrimaryHitDistReady ||
        (m_rtOutputWidth != width) ||
        !m_rtAovHitDistRecons ||
        !m_rtAovHitDistReconsReady ||
        !m_rtAovHitDistReconsConf ||
        !m_rtAovHitDistReconsConfReady ||
        !m_rtHistoryHitDist[0] ||
        !m_rtHistoryHitDist[1] ||
        !m_rtHistoryHitDistConf[0] ||
        !m_rtHistoryHitDistConf[1] ||
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
    m_rtAovPrimaryHitDistReady = false;
    m_rtAovPrimaryHitDist.Reset();
    m_rtAovHitDistReconsReady = false;
    m_rtAovHitDistReconsConfReady = false;
    m_rtAovHitDistRecons.Reset();
    m_rtAovHitDistReconsConf.Reset();

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
                IID_PPV_ARGS(&m_rtAovPrimaryHitDist)),
            "Create RT AOV primary hit distance");

        SetD3D12ObjectName(
            m_rtAovPrimaryHitDist.Get(),
            L"RT AOV Primary Hit Distance");

        CommandList::SetGlobalState(
            m_rtAovPrimaryHitDist.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        device->CreateUnorderedAccessView(
            m_rtAovPrimaryHitDist.Get(),
            nullptr,
            &uav,
            RtUavCpuAt(6));
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

    CreateR16UavTexture(
        m_rtAovHitDistRecons,
        L"RT AOV Hit Distance Reconstructed");

    CreateR16UavTexture(
        m_rtAovHitDistReconsConf,
        L"RT AOV Hit Distance Reconstructed Confidence");

    m_rtAovHitDistReconsReady = true;
    m_rtAovHitDistReconsConfReady = true;
    m_rtAovPrimaryHitDistReady = true;
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
    uint32_t& tableSrvCount)
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

    if (!table.IsValid() || tableSrvCount != kRtDenoiseSrvCount)
    {
        table = m_srvHeap.Allocate(kRtDenoiseSrvCount);
        tableSrvCount = kRtDenoiseSrvCount;
    }


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

    return true;
}

void Renderer::CreateRtHistoryResources(ID3D12Device* device, uint32_t width, uint32_t height)
{
    m_rtTemporalHistoryValid = false;
    m_rtHistoryReadIndex = 0;

    for (auto& r : m_rtHistoryAccum)  r.Reset();
    for (auto& r : m_rtHistoryNormal) r.Reset();
    for (auto& r : m_rtHistoryDepth)  r.Reset();
    for (auto& r : m_rtHistoryMoments) r.Reset();
    for (auto& r : m_rtSvgfPing) r.Reset();
    for (auto& r : m_rtHistorySpec)             r.Reset();
    for (auto& r : m_rtHistorySpecResp)         r.Reset();
    for (auto& r : m_rtHistoryMomentsSpec)      r.Reset();
    for (auto& r : m_rtHistoryMomentsSpecResp)  r.Reset();

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
            m_rtHistoryHitDist[i].Reset();
            m_rtHistoryHitDistConf[i].Reset();

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
                    IID_PPV_ARGS(&m_rtHistoryHitDist[i])),
                "Create RT history hit distance");

            SetD3D12ObjectName(
                m_rtHistoryHitDist[i].Get(),
                i == 0 ? L"RT History Hit Distance 0" : L"RT History Hit Distance 1");

            CommandList::SetGlobalState(
                m_rtHistoryHitDist[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            ThrowIfFailed(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr,
                    IID_PPV_ARGS(&m_rtHistoryHitDistConf[i])),
                "Create RT history hit distance confidence");

            SetD3D12ObjectName(
                m_rtHistoryHitDistConf[i].Get(),
                i == 0 ? L"RT History Hit Distance Confidence 0" : L"RT History Hit Distance Confidence 1");

            CommandList::SetGlobalState(
                m_rtHistoryHitDistConf[i].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    m_rtHitDistHistoryValid = false;
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
    if (!device || !diffuseResource || !specularResource)
        return false;

    auto& table = m_rtFrames[frameIndex].combineSrvTable;

    if (!table.IsValid())
        table = m_srvHeap.Allocate(2);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto CombineCpuHandle = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = table.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.MostDetailedMip = 0;

    device->CreateShaderResourceView(
        diffuseResource,
        &srv,
        CombineCpuHandle(0));

    device->CreateShaderResourceView(
        specularResource,
        &srv,
        CombineCpuHandle(1));

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
    DescriptorAllocator::Allocation& srvTable,
    DescriptorAllocator::Allocation& uavTable)
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
        !m_rtAccumDiffuseReady ||
        !m_rtAccumSpecReady ||
        !m_rtAovReady ||
        !m_rtOutput)
    {
        return false;
    }

    if (!srvTable.IsValid())
        srvTable = m_srvHeap.Allocate(9);

    if (!uavTable.IsValid())
        uavTable = m_srvHeap.Allocate(3);

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

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtTemporalConstants(
    uint32_t frameIndex, 
    uint32_t width, 
    uint32_t height,
    float temporalAlpha,
    float roughnessSigma,
    float motionConfMin,
    float motionConfPower)
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
    cb->motionConfMin = std::max(0.0f, std::min(1.0f, motionConfMin));
    cb->motionConfPower = std::max(1e-3f, motionConfPower);

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
    ID3D12Resource* motionConfResource)
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

    if (!table.IsValid() || tableSrvCount != kRtSvgfSrvCount)
    {
        table = m_srvHeap.Allocate(kRtSvgfSrvCount);
        tableSrvCount = kRtSvgfSrvCount;
    }

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
    float motionConfMin)
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
    cb->debugView = m_debugView;
    cb->lengthSkipThreshold = std::clamp(m_rtAtrousLengthSkipThreshold, 0.0f, 1.0f);
    cb->enableLengthSkip = m_rtAtrousEnableLengthSkip ? 1u : 0u;

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

    if (!frame.historySelectSrvTable.IsValid() ||
        frame.historySelectSrvCount != kRtHistorySelectSrvCount)
    {
        frame.historySelectSrvTable = m_srvHeap.Allocate(kRtHistorySelectSrvCount);
        frame.historySelectSrvCount = kRtHistorySelectSrvCount;
    }

    if (!frame.historySelectUavTable.IsValid() ||
        frame.historySelectUavCount != kRtHistorySelectUavCount)
    {
        frame.historySelectUavTable = m_srvHeap.Allocate(kRtHistorySelectUavCount);
        frame.historySelectUavCount = kRtHistorySelectUavCount;
    }

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
    float motionConfPower)
{
    if (!device ||
        !inputSignal ||
        !outputResource ||
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
    cl.FlushBarriers();

    const bool okTable =
        UpdateRtDenoiseSrvTable(
            frameIndex,
            device,
            inputSignal,
            srvTable,
            srvTableCount);

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
    bool useReconstructedHitDist)
{
    ID3D12Resource* hitDistResource =
        (useReconstructedHitDist &&
            m_rtAovHitDistRecons &&
            m_rtAovHitDistReconsReady)
        ? m_rtAovHitDistRecons.Get()
        : m_rtAovPrimaryHitDist.Get();

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
        !m_rtAovPrimaryHitDist ||
        !m_rtAovPrimaryHitDistReady ||
        !hitDistResource ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    if (!frame.motionDilateSrvTable.IsValid() ||
        frame.motionDilateSrvCount != kRtMotionDilateSrvCount)
    {
        frame.motionDilateSrvTable = m_srvHeap.Allocate(kRtMotionDilateSrvCount);
        frame.motionDilateSrvCount = kRtMotionDilateSrvCount;
    }

    if (!frame.motionDilateUavTable.IsValid())
        frame.motionDilateUavTable = m_srvHeap.Allocate(3);

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

    // t3 =  primary hit distance
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(
            hitDistResource,
            &srv,
            SrvAt(3));
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
    bool useReconstructedHitDist)
{
    ID3D12Resource* hitDistResource =
        (useReconstructedHitDist &&
            m_rtAovHitDistRecons &&
            m_rtAovHitDistReconsReady)
        ? m_rtAovHitDistRecons.Get()
        : m_rtAovPrimaryHitDist.Get();

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
        !m_rtAovPrimaryHitDist ||
        !m_rtAovPrimaryHitDistReady ||
        !hitDistResource || 
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
    cl.Transition(hitDistResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    const bool okTables =
        UpdateRtMotionDilateTables(
            frameIndex,
            device,
            useReconstructedHitDist);

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

    return alloc.gpu;
}

bool Renderer::UpdateRtHitDistReconstructTables(
    uint32_t frameIndex,
    ID3D12Device* device)
{
    if (!device ||
        !m_rtAovPrimaryHitDist ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovMotion ||
        !m_rtHistoryHitDist[0] ||
        !m_rtHistoryHitDist[1] ||
        !m_rtHistoryDepth[0] ||
        !m_rtHistoryDepth[1] ||
        !m_rtHistoryNormal[0] ||
        !m_rtHistoryNormal[1] ||
        !m_rtAovHitDistRecons ||
        !m_rtAovHitDistReconsConf ||
        !m_rtOutput ||
        !m_rtAovPrimaryHitDistReady ||
        !m_rtAovMotionReady ||
        !m_rtAovHitDistReconsReady ||
        !m_rtAovHitDistReconsConfReady ||
        !m_rtAovReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    if (!frame.hitDistReconstructSrvTable.IsValid() ||
        frame.hitDistReconstructSrvCount != kRtHitDistReconstructSrvCount)
    {
        frame.hitDistReconstructSrvTable =
            m_srvHeap.Allocate(kRtHitDistReconstructSrvCount);
        frame.hitDistReconstructSrvCount = kRtHitDistReconstructSrvCount;
    }

    if (!frame.hitDistReconstructUavTable.IsValid() ||
        frame.hitDistReconstructUavCount != kRtHitDistReconstructUavCount)
    {
        frame.hitDistReconstructUavTable =
            m_srvHeap.Allocate(kRtHitDistReconstructUavCount);
        frame.hitDistReconstructUavCount = kRtHitDistReconstructUavCount;
    }

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.hitDistReconstructSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.hitDistReconstructUavTable.cpu;
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

    // t0 = raw primary hit distance
    WriteR16Srv(m_rtAovPrimaryHitDist.Get(), 0);

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

    // t4 = previous reconstructed hit distance
    WriteR16Srv(m_rtHistoryHitDist[readIndex].Get(), 4);

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

    // u0 = reconstructed hit distance
    WriteR16Uav(m_rtAovHitDistRecons.Get(), 0);

    // u1 = reconstructed confidence
    WriteR16Uav(m_rtAovHitDistReconsConf.Get(), 1);

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

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtHitDistReconstructConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    auto alloc = m_upload.Allocate(
        frameIndex,
        sizeof(RtHitDistReconstructConstants),
        256);

    auto* cb = reinterpret_cast<RtHitDistReconstructConstants*>(alloc.cpu);
    *cb = {};

    cb->invResolution = {
        1.0f / static_cast<float>(width),
        1.0f / static_cast<float>(height)
    };

    cb->alpha =
        std::max(0.0f, std::min(1.0f, m_rtHitDistReconsAlpha));

    cb->depthSigma = 0.02f;
    cb->normalSigma = 0.25f;
    cb->roughnessSigma = 0.20f;

    cb->historyValid = m_rtHitDistHistoryValid ? 1u : 0u;
    cb->debugView = m_debugView;
    cb->radius = 2u;
    cb->hitDistVisMax = 25.0f;

    return alloc.gpu;
}

bool Renderer::RunRtHitDistReconstruct(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height)
{
    if (!device ||
        !m_rtAovPrimaryHitDist ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovMotion ||
        !m_rtAovHitDistRecons ||
        !m_rtAovHitDistReconsConf ||
        !m_rtOutput ||
        !m_rtAovPrimaryHitDistReady ||
        !m_rtAovMotionReady ||
        !m_rtAovReady ||
        !m_rtAovHitDistReconsReady ||
        !m_rtAovHitDistReconsConfReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT Hit Distance Reconstruct");

    cl.Transition(m_rtAovPrimaryHitDist.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovMotion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryHitDist[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryDepth[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryNormal[m_rtHistoryReadIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtAovHitDistRecons.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtAovHitDistReconsConf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.FlushBarriers();

    const bool okTables =
        UpdateRtHitDistReconstructTables(
            frameIndex,
            device);

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtHitDistReconstructConstants(
            frameIndex,
            width,
            height);

    m_rtHitDistReconstructPass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].hitDistReconstructSrvTable.gpu,
        m_rtFrames[frameIndex].hitDistReconstructUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[3]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = m_rtAovHitDistRecons.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rtAovHitDistReconsConf.Get();

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(3, barriers);

    CmdEndEvent(cmdList);
    return true;
}

void Renderer::CommitRtHitDistHistory(
    CommandList& cl,
    uint32_t writeIndex)
{
    if (writeIndex >= 2 ||
        !m_rtAovHitDistRecons ||
        !m_rtAovHitDistReconsConf ||
        !m_rtHistoryHitDist[writeIndex] ||
        !m_rtHistoryHitDistConf[writeIndex] ||
        !m_rtAovHitDistReconsReady ||
        !m_rtAovHitDistReconsConfReady)
    {
        m_rtHitDistHistoryValid = false;
        return;
    }

    auto* cmdList = cl.Get();

    cl.Transition(m_rtAovHitDistRecons.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl.Transition(m_rtAovHitDistReconsConf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl.Transition(m_rtHistoryHitDist[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    cl.Transition(m_rtHistoryHitDistConf[writeIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    cl.FlushBarriers();

    cmdList->CopyResource(
        m_rtHistoryHitDist[writeIndex].Get(),
        m_rtAovHitDistRecons.Get());

    cmdList->CopyResource(
        m_rtHistoryHitDistConf[writeIndex].Get(),
        m_rtAovHitDistReconsConf.Get());

    cl.Transition(m_rtHistoryHitDist[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtHistoryHitDistConf[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.FlushBarriers();

    m_rtHitDistHistoryValid = true;
}
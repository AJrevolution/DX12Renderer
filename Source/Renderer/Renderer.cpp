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

    constexpr uint32_t kDebugReqDxr =
        DebugViewReq_DxrSupport |
        DebugViewReq_RaytracingEnabled |
        DebugViewReq_DxrPipeline;

    constexpr uint32_t kDebugReqRtOutput =
        kDebugReqDxr |
        DebugViewReq_RtResources;

    constexpr uint32_t kDebugReqRtAov =
        kDebugReqRtOutput |
        DebugViewReq_RtAovs;

    constexpr uint32_t kDebugReqRtPost =
        kDebugReqRtAov |
        DebugViewReq_RtHistory |
        DebugViewReq_RtPostStack;

    constexpr uint32_t kDebugReqRtRestir =
        kDebugReqRtAov |
        DebugViewReq_Restir;

    constexpr float kOrbitAutoYawSpeed = 0.35f;
    constexpr float kOrbitManualYawSpeed = 1.25f;
    constexpr float kOrbitManualZoomSpeed = 4.0f;

    constexpr float kOrbitMinRadius = 1.0f;
    constexpr float kOrbitMaxRadius = 20.0f;
    constexpr float kOrbitTargetY = 0.5f;
    constexpr float kOrbitCameraHeightOffset = 1.5f;

    constexpr float kFreeRoamMoveSpeed = 3.0f;
    constexpr float kFreeRoamMouseSensitivity = 0.0025f;
    constexpr float kFreeRoamMinPitch = -1.45f;
    constexpr float kFreeRoamMaxPitch = 1.45f;

    constexpr float kCameraMaxDeltaSeconds = 0.10f;
    constexpr float kSceneAnimationMaxDeltaSeconds = 0.10f;

    constexpr uint32_t kImportedModelRtObjectIdBase = 1000u;

    constexpr uint32_t kRtMeshTypeFloor = 0u;
    constexpr uint32_t kRtMeshTypeQuad = 1u;
    constexpr uint32_t kRtMeshTypeImported = 2u;

    constexpr uint32_t kRtMaxMaterials = 64u;
    constexpr uint32_t kRtTexturesPerMaterial = 4u;

    constexpr uint32_t kRtBaseColorTextureSlot = 0u;
    constexpr uint32_t kRtNormalTextureSlot = 1u;
    constexpr uint32_t kRtMetalRoughTextureSlot = 2u;
    constexpr uint32_t kRtOcclusionTextureSlot = 3u;

    constexpr uint32_t kRtRegisterQuadVerts = 1u;
    constexpr uint32_t kRtRegisterQuadIndices = 2u;
    constexpr uint32_t kRtRegisterFloorVerts = 3u;
    constexpr uint32_t kRtRegisterFloorIndices = 4u;
    constexpr uint32_t kRtRegisterInstanceData = 5u;
    constexpr uint32_t kRtRegisterMaterialTextures = 6u;

    constexpr uint32_t kRtRegisterImportedVerts = 270u;
    constexpr uint32_t kRtRegisterImportedIndices = 271u;

    constexpr uint32_t kRtRegisterBrdfLut = 280u;
    constexpr uint32_t kRtRegisterIblDiffuse = 281u;
    constexpr uint32_t kRtRegisterIblSpecular = 282u;
    constexpr uint32_t kRtRegisterEnvAlias = 283u;
    constexpr uint32_t kRtRegisterRestirResolveReservoir = 284u;

    // racetrackentire.gltf currently has its lowest world-space Y around 1.772.
    // Move it down so the imported ground sits around renderer Y=0.
    // This is a temporary phase placement constant; a scene manifest should own
    // model placement later.
    constexpr float kDefaultImportedModelYOffset = -1.77210808f;

    constexpr uint32_t kRtHighestSrvRegister =
        kRtRegisterRestirResolveReservoir;

    // Descriptor table starts at t1, so 284 descriptors covers t1..t284.
    constexpr uint32_t kRtSrvTableCount =
        kRtHighestSrvRegister;

    static_assert(kRtSrvTableCount == 284u);
    static_assert(
        kRtRegisterMaterialTextures +
        kRtMaxMaterials * kRtTexturesPerMaterial - 1u == 261u);

    float WrapAngleRadians(float angle)
    {
        constexpr float twoPi = 6.28318530717958647692f;

        if (angle > twoPi || angle < -twoPi)
            return std::fmod(angle, twoPi);

        return angle;
    }

    DirectX::XMVECTOR FreeRoamForwardVector(float yaw, float pitch)
    {
        const float cp = cosf(pitch);

        return DirectX::XMVector3Normalize(DirectX::XMVectorSet(
            sinf(yaw) * cp,
            sinf(pitch),
            -cosf(yaw) * cp,
            0.0f));
    }

    DirectX::XMVECTOR FreeRoamRightVector(float yaw)
    {
        const DirectX::XMVECTOR forward =
            FreeRoamForwardVector(yaw, 0.0f);

        const DirectX::XMVECTOR up =
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        return DirectX::XMVector3Normalize(
            DirectX::XMVector3Cross(forward, up));
    }

    bool HasDebugViewRequirement(const DebugViewDesc& desc, uint32_t requirement)
    {
        return (desc.requirements & requirement) != 0;
    }

    constexpr DebugViewDesc kDebugViewDescs[] =
    {
        { 0, "Final Shaded Output", "Final", DebugViewDomain::Final, DebugViewReq_None, false, false, false, false },

        { 18, "Reprojection Validity", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 19, "Rejection / Disocclusion", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 20, "Chosen Previous UV", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 21, "History Current Difference", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 22, "History Length", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 23, "Temporal Alpha", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 24, "Temporal Moments Variance", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 25, "History Warm-Up / Convergence", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 26, "Depth Reprojection Error", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 27, "Stored Guide Roughness", "DXR AOV", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 28, "Roughness / Specular Protection Proxy", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 29, "Final History Selector Mask", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 30, "Stable History Signal", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 31, "Responsive History Signal", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 32, "Spec Direction Reuse Mask", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 33, "Spec Direction Dot", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 34, "Reprojection Search Chosen Offset", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 35, "Reprojection Best Score", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 36, "Confidence-Scaled Alpha", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 37, "Roughness Selector Vote", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 38, "Length Selector Vote", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 39, "Final Selector Value", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 40, "Stable History Length", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 41, "Responsive History Length", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 42, "Selected History Length", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 43, "Centre History Length Attenuation", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 44, "Wide Iteration Skip Mask", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 45, "Variance-Normalised Signal", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 46, "Reprojection Best Score", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 47, "Final Alpha After Variance Shaping", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 48, "Diffuse Accumulation", "DXR Split", DebugViewDomain::DXR, kDebugReqRtOutput, true, true, true, true },
        { 49, "Specular Accumulation", "DXR Split", DebugViewDomain::DXR, kDebugReqRtOutput, true, true, true, true },
        { 50, "Diffuse + Specular Accumulation", "DXR Split", DebugViewDomain::DXR, kDebugReqRtOutput, true, true, true, true },
        { 51, "Stored Previous UV", "DXR Motion", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 52, "Invalid Previous UV Mask", "DXR Motion", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 53, "Raw Previous UV Invalid Mask", "DXR Motion", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 54, "Dilated Previous UV Invalid Mask", "RT Motion Dilation", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 55, "Motion Confidence", "RT Motion Dilation", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 56, "Temporal Motion Confidence", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 57, "Alpha After Motion-Confidence Scaling", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 58, "Post-Power Motion Confidence", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 59, "Selected Spec Variance", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 60, "Spec A-Trous Shaped Motion Confidence", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 61, "Raw ViewZ Heatmap", "DXR ViewZ", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 62, "Invalid Raw ViewZ Mask", "DXR ViewZ", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 63, "Reconstructed ViewZ Heatmap", "RT ViewZ Reconstruction", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 64, "Reconstructed ViewZ Confidence", "RT ViewZ Reconstruction", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 65, "SurfaceId Visualisation", "DXR SurfaceId", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 66, "Invalid SurfaceId Mask", "DXR SurfaceId", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 67, "Diffuse Albedo Visualisation", "DXR Diffuse Albedo", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 68, "Invalid / Near-Zero Diffuse Albedo Mask", "DXR Diffuse Albedo", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 69, "Demodulated Diffuse Lighting", "RT Diffuse Demodulation", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 70, "Demodulation Instability Mask", "RT Diffuse Demodulation", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 71, "Temporal Hit-Distance Weight", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 72, "Spec History Mismatch Mask", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 73, "Spec A-Trous Hit-Distance Weight", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 74, "SurfaceId Visualisation", "DXR SurfaceId", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },
        { 75, "Invalid SurfaceId Mask", "DXR SurfaceId", DebugViewDomain::RT_AOV, kDebugReqRtAov, true, true, true, true },

        // Present in Renderer.cpp routing, but not documented in the locked
        // Renderer.h namespace comment. Preserve as current routed IDs.
        { 76, "Temporal Debug 76", "RT Temporal / Current Routing", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 77, "Temporal Debug 77", "RT Temporal / Current Routing", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 78, "A-Trous Output Debug 78", "RT A-Trous / Current Routing", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },

        { 79, "Normalised Reconstructed ViewZ", "RT ViewZ Reconstruction", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 80, "Invalid Normalised ViewZ Mask", "RT ViewZ Reconstruction", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 81, "Diffuse Outlier Clamp Factor", "RT Outlier Clamp", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 82, "Specular Outlier Clamp Factor", "RT Outlier Clamp", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 83, "Invalid / NaN / Inf Sanitised Mask", "RT Outlier Clamp", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 84, "Temporal History Colour Clamp Amount", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 85, "Moment Variance Clamp Mask", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 86, "Outlier Neighbourhood Valid Weight", "RT Outlier Clamp", DebugViewDomain::RT_GuideReconstruct, kDebugReqRtAov, true, true, true, true },
        { 87, "Temporal Signal Confidence", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 88, "Temporal Anti-Lag Responsiveness", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 89, "Confidence-Shaped History Length", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 90, "Temporal Luminance Delta Confidence", "RT Temporal", DebugViewDomain::RT_Temporal, kDebugReqRtPost, true, true, true, true },
        { 91, "Spec Responsive Selection Weight", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 92, "Spec Stable History Confidence", "RT History Select", DebugViewDomain::RT_HistorySelect, kDebugReqRtPost, true, true, true, true },
        { 93, "Adaptive Blur Strength", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 94, "Wide-Iteration Suppression Mask", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 95, "Variance / History Instability", "RT A-Trous", DebugViewDomain::RT_Spatial, kDebugReqRtPost, true, true, true, true },
        { 96, "Environment Alias / PDF Heatmap", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 97, "Sampled Environment Direction / PDF", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 98, "Environment NEE MIS Weight", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 99, "Environment Visibility Mask", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 100, "Direct Environment NEE Luminance", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 101, "BRDF Environment-Hit Luminance Approximation", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 102, "Environment Sampling Technique / Mode", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 103, "Environment Sampling Fallback / Readiness", "RT Environment Sampling", DebugViewDomain::RT_Sampling, kDebugReqRtOutput, true, true, true, true },
        { 104, "Initial ReSTIR Target Luminance", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 105, "Initial ReSTIR Source PDF", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 106, "Temporal ReSTIR Reuse Accepted Mask", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 107, "Temporal ReSTIR M / Confidence", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 108, "Spatial ReSTIR Accepted Neighbour Count", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 109, "Spatial ReSTIR Selected Neighbour Distance", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 110, "Resolved ReSTIR Reservoir Final W", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 111, "Resolved ReSTIR Visibility Mask", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 112, "Resolved ReSTIR Diffuse Luminance", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 113, "Resolved ReSTIR Specular Luminance", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
        { 114, "Resolved ReSTIR Invalid Reason Mask", "RT ReSTIR Env DI", DebugViewDomain::RT_ReSTIR, kDebugReqRtRestir, true, true, true, true },
    };

    DirectX::XMFLOAT3 SubFloat3(
        DirectX::XMFLOAT3 a,
        DirectX::XMFLOAT3 b)
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    DirectX::XMFLOAT3 AddFloat3(
        DirectX::XMFLOAT3 a,
        DirectX::XMFLOAT3 b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    DirectX::XMFLOAT3 MinFloat3(
        DirectX::XMFLOAT3 a,
        DirectX::XMFLOAT3 b)
    {
        return
        {
            std::min(a.x, b.x),
            std::min(a.y, b.y),
            std::min(a.z, b.z)
        };
    }

    DirectX::XMFLOAT3 MaxFloat3(
        DirectX::XMFLOAT3 a,
        DirectX::XMFLOAT3 b)
    {
        return
        {
            std::max(a.x, b.x),
            std::max(a.y, b.y),
            std::max(a.z, b.z)
        };
    }

    DirectX::XMFLOAT3 MulFloat3(
        DirectX::XMFLOAT3 a,
        float s)
    {
        return { a.x * s, a.y * s, a.z * s };
    }

}

const DebugViewDesc* FindDebugViewDesc(uint32_t id)
{
    for (const DebugViewDesc& desc : kDebugViewDescs)
    {
        if (desc.id == id)
            return &desc;
    }

    return nullptr;
}

const DebugViewDesc* GetDebugViewDescs(std::size_t& count)
{
    count = sizeof(kDebugViewDescs) / sizeof(kDebugViewDescs[0]);
    return kDebugViewDescs;
}

const char* DebugViewAvailabilityName(DebugViewAvailability availability)
{
    switch (availability)
    {
    case DebugViewAvailability::Available:
        return "Available";
    case DebugViewAvailability::Unavailable:
        return "Unavailable";
    case DebugViewAvailability::RequiresDxrSupport:
        return "Requires DXR support";
    case DebugViewAvailability::RequiresRaytracingEnabled:
        return "Requires raytracing enabled";
    case DebugViewAvailability::RequiresDxrPipeline:
        return "Requires DXR pipeline";
    case DebugViewAvailability::PendingResources:
        return "Pending resources";
    case DebugViewAvailability::PendingHistory:
        return "Pending history";
    case DebugViewAvailability::RequiresRestir:
        return "Requires ReSTIR";
    case DebugViewAvailability::RequiresGBuffer:
        return "Requires G-buffer";
    case DebugViewAvailability::RequiresShadow:
        return "Requires shadow";
    case DebugViewAvailability::UnknownId:
        return "Unknown debug view";
    default:
        return "Unknown availability";
    }
}

const char* CameraControlModeName(CameraControlMode mode)
{
    switch (mode)
    {
    case CameraControlMode::AutoOrbit:
        return "Auto orbit";
    case CameraControlMode::ManualOrbit:
        return "Manual orbit";
    case CameraControlMode::FreeRoam:
        return "Free roam";
    default:
        return "Unknown camera mode";
    }
}

DebugViewAvailability Renderer::GetDebugViewAvailability(uint32_t id) const
{
    const DebugViewDesc* desc = FindDebugViewDesc(id);
    if (!desc)
        return DebugViewAvailability::UnknownId;

    if (id == 0)
        return DebugViewAvailability::Available;

    if (HasDebugViewRequirement(*desc, DebugViewReq_DxrSupport) &&
        !m_dxrAvailable)
    {
        return DebugViewAvailability::RequiresDxrSupport;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_RaytracingEnabled) &&
        !m_useRaytracing)
    {
        return DebugViewAvailability::RequiresRaytracingEnabled;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_DxrPipeline) &&
        (!m_device5 || !m_rtPipeline.StateObject()))
    {
        return DebugViewAvailability::RequiresDxrPipeline;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_GBuffer) &&
        !m_gbufferReady)
    {
        return DebugViewAvailability::RequiresGBuffer;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_Shadow) &&
        !m_shadowReady)
    {
        return DebugViewAvailability::RequiresShadow;
    }

    // The checks below intentionally describe "ready this exact frame".
    // They are not selection gates. RT output, AOV, history, and ReSTIR
    // resources may be created or populated later in the same valid DXR frame
    // by EnsureRtOutputSize(), EnsureRtRestirResources(), and existing
    // pass-level readiness checks.
    if (HasDebugViewRequirement(*desc, DebugViewReq_RtResources) &&
        (!m_rtOutputReady ||
            !m_rtOutput ||
            !m_rtAccumDiffuseReady ||
            !m_rtAccumSpecReady))
    {
        return DebugViewAvailability::PendingResources;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_RtAovs) &&
        (!m_rtAovReady ||
            !m_rtAovNormal ||
            !m_rtAovDepth))
    {
        return DebugViewAvailability::PendingResources;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_RtPostStack) &&
        (!m_rtPostReady ||
            !m_rtPostDiffuse ||
            !m_rtPostSpec))
    {
        return DebugViewAvailability::PendingResources;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_RtHistory) &&
        (!m_rtHistoryAccum[0] ||
            !m_rtHistoryAccum[1] ||
            !m_rtHistoryNormal[0] ||
            !m_rtHistoryNormal[1] ||
            !m_rtHistoryDepth[0] ||
            !m_rtHistoryDepth[1]))
    {
        return DebugViewAvailability::PendingHistory;
    }

    if (HasDebugViewRequirement(*desc, DebugViewReq_Restir) &&
        !m_rtRestirResourcesReady)
    {
        return DebugViewAvailability::PendingResources;
    }

    return DebugViewAvailability::Available;
}

bool Renderer::IsDebugViewSelectable(uint32_t id) const
{
    // PendingResources and PendingHistory are selectable by design.
    // Valid DXR debug views may need to be selected before EnsureRtOutputSize(),
    // EnsureRtRestirResources(), temporal history allocation, or the first DXR
    // frame has populated their resources. Only hard platform/mode failures
    // such as missing DXR support, raytracing disabled, missing pipeline, or
    // unknown IDs should block selection here.
    switch (GetDebugViewAvailability(id))
    {
    case DebugViewAvailability::Available:
    case DebugViewAvailability::PendingResources:
    case DebugViewAvailability::PendingHistory:
        return true;

    default:
        return false;
    }
}

bool Renderer::SetDebugView(uint32_t id)
{
    if (!IsDebugViewSelectable(id))
        return false;

    m_debugView = id;
    return true;
}

uint32_t Renderer::GetDebugView() const
{
    return m_debugView;
}

bool Renderer::IsRaytracingEnabled() const
{
    return m_useRaytracing;
}

void Renderer::SetRaytracingEnabled(bool enabled)
{
    m_useRaytracing = enabled;

    if (!IsDebugViewSelectable(m_debugView))
    {
        m_debugView = 0;
    }
}

bool Renderer::IsRtAccumulationEnabled() const
{
    return m_rtAccumulate;
}

void Renderer::SetRtAccumulationEnabled(bool enabled)
{
    if (m_rtAccumulate == enabled)
        return;

    m_rtAccumulate = enabled;

    // Accumulation mode changes alter the meaning of the RT accumulation
    // targets. Reset immediately so the next frame starts from a clean sample.
    ResetRtAccumulation(true);
}

void Renderer::ToggleRtAccumulation()
{
    SetRtAccumulationEnabled(!m_rtAccumulate);
}

CameraControlMode Renderer::GetCameraControlMode() const
{
    return m_cameraMode;
}

void Renderer::SetCameraControlMode(CameraControlMode mode)
{
    if (m_cameraMode == mode)
        return;

    const CameraControlMode oldMode = m_cameraMode;

    if (mode == CameraControlMode::FreeRoam)
    {
        InitialiseFreeRoamFromOrbitCamera();
    }
    else if (oldMode == CameraControlMode::FreeRoam)
    {
        ProjectOrbitCameraFromFreeRoam();
    }

    m_cameraMode = mode;

    // Reset the camera time baseline whenever the mode changes. This prevents
    // auto-orbit from snapping or consuming a large accumulated time delta when
    // returning from manual/free camera control.
    m_cameraTimeValid = false;
    ++m_cameraRevision;
}

bool Renderer::IsAutoOrbitEnabled() const
{
    return m_cameraMode == CameraControlMode::AutoOrbit;
}

void Renderer::SetAutoOrbitEnabled(bool enabled)
{
    SetCameraControlMode(
        enabled
        ? CameraControlMode::AutoOrbit
        : CameraControlMode::ManualOrbit);
}

void Renderer::ToggleAutoOrbit()
{
    SetAutoOrbitEnabled(!IsAutoOrbitEnabled());
}

bool Renderer::IsFreeRoamEnabled() const
{
    return m_cameraMode == CameraControlMode::FreeRoam;
}

void Renderer::SetFreeRoamEnabled(bool enabled)
{
    SetCameraControlMode(
        enabled
        ? CameraControlMode::FreeRoam
        : CameraControlMode::ManualOrbit);
}

void Renderer::ToggleFreeRoam()
{
    SetFreeRoamEnabled(!IsFreeRoamEnabled());
}

void Renderer::ApplyOrbitCameraInput(const OrbitCameraInput& input)
{
    const float dt = std::clamp(
        input.deltaSeconds,
        0.0f,
        kCameraMaxDeltaSeconds);

    const float yawAxis = std::clamp(input.yawAxis, -1.0f, 1.0f);
    const float zoomAxis = std::clamp(input.zoomAxis, -1.0f, 1.0f);

    if (dt <= 0.0f || (yawAxis == 0.0f && zoomAxis == 0.0f))
        return;

    SetCameraControlMode(CameraControlMode::ManualOrbit);

    const float oldYaw = m_camYaw;
    const float oldRadius = m_camRadius;

    m_camYaw = WrapAngleRadians(
        m_camYaw + yawAxis * kOrbitManualYawSpeed * dt);

    m_camRadius = std::clamp(
        m_camRadius + zoomAxis * kOrbitManualZoomSpeed * dt,
        kOrbitMinRadius,
        kOrbitMaxRadius);

    if (oldYaw != m_camYaw || oldRadius != m_camRadius)
    {
        ++m_cameraRevision;
    }
}

void Renderer::ApplyFreeRoamCameraInput(const FreeRoamCameraInput& input)
{
    if (m_cameraMode != CameraControlMode::FreeRoam)
        return;

    const float dt = std::clamp(
        input.deltaSeconds,
        0.0f,
        kCameraMaxDeltaSeconds);

    const float forwardAxis = std::clamp(input.moveForwardAxis, -1.0f, 1.0f);
    const float rightAxis = std::clamp(input.moveRightAxis, -1.0f, 1.0f);

    const float mouseDeltaX = std::clamp(input.mouseDeltaX, -1000.0f, 1000.0f);
    const float mouseDeltaY = std::clamp(input.mouseDeltaY, -1000.0f, 1000.0f);

    bool changed = false;

    if (mouseDeltaX != 0.0f || mouseDeltaY != 0.0f)
    {
        m_freeCamYaw = WrapAngleRadians(
            m_freeCamYaw - mouseDeltaX * kFreeRoamMouseSensitivity);

        m_freeCamPitch = std::clamp(
            m_freeCamPitch - mouseDeltaY * kFreeRoamMouseSensitivity,
            kFreeRoamMinPitch,
            kFreeRoamMaxPitch);

        changed = true;
    }

    if (dt > 0.0f && (forwardAxis != 0.0f || rightAxis != 0.0f))
    {
        using namespace DirectX;

        const XMVECTOR forward =
            FreeRoamForwardVector(m_freeCamYaw, m_freeCamPitch);

        const XMVECTOR right =
            FreeRoamRightVector(m_freeCamYaw);

        XMVECTOR position =
            XMLoadFloat3(&m_freeCamPosition);

        const float moveDistance = kFreeRoamMoveSpeed * dt;

        position += forward * (forwardAxis * moveDistance);
        position += right * (rightAxis * moveDistance);

        XMStoreFloat3(&m_freeCamPosition, position);

        changed = true;
    }

    if (changed)
    {
        ++m_cameraRevision;
    }
}

void Renderer::ComputeOrbitCamera(
    DirectX::XMFLOAT3& outPosition,
    DirectX::XMFLOAT3& outTarget) const
{
    const float cp = cosf(m_camPitch);
    const float sp = sinf(m_camPitch);
    const float cy = cosf(m_camYaw);
    const float sy = sinf(m_camYaw);

    outPosition =
    {
        sy * cp * m_camRadius,
        sp * m_camRadius + kOrbitCameraHeightOffset,
        cy * cp * m_camRadius
    };

    outTarget = { 0.0f, kOrbitTargetY, 0.0f };
}

void Renderer::InitialiseFreeRoamFromOrbitCamera()
{
    using namespace DirectX;

    XMFLOAT3 position{};
    XMFLOAT3 target{};
    ComputeOrbitCamera(position, target);

    m_freeCamPosition = position;

    XMVECTOR forward =
        XMLoadFloat3(&target) - XMLoadFloat3(&position);

    forward = XMVector3Normalize(forward);

    XMFLOAT3 forward3{};
    XMStoreFloat3(&forward3, forward);

    m_freeCamPitch = asinf(
        std::clamp(forward3.y, -1.0f, 1.0f));

    m_freeCamYaw = atan2f(forward3.x, -forward3.z);
    m_freeCamYaw = WrapAngleRadians(m_freeCamYaw);

    m_freeCamInitialised = true;
}

void Renderer::ProjectOrbitCameraFromFreeRoam()
{
    if (!m_freeCamInitialised)
        return;

    const float x = m_freeCamPosition.x;
    const float z = m_freeCamPosition.z;
    const float y = m_freeCamPosition.y - kOrbitCameraHeightOffset;

    const float radius =
        sqrtf(x * x + y * y + z * z);

    m_camRadius = std::clamp(
        radius,
        kOrbitMinRadius,
        kOrbitMaxRadius);

    if (m_camRadius > 1e-4f)
    {
        m_camYaw = WrapAngleRadians(atan2f(x, z));

        m_camPitch = asinf(std::clamp(
            y / m_camRadius,
            -0.95f,
            0.95f));
    }
}

bool Renderer::ShouldAdvanceSceneAnimation() const
{
    // Auto-orbit is presentation mode: camera and demo animation both advance.
    // Manual orbit and free-roam are inspection modes: the scene freezes so
    // progressive RT accumulation can converge once the camera stops moving.
    return m_cameraMode == CameraControlMode::AutoOrbit;
}

float Renderer::UpdateSceneAnimationTime(float frameTime)
{
    if (!m_sceneAnimationTimeValid)
    {
        m_sceneAnimationTime = frameTime;
        m_lastSceneAnimationUpdateTime = frameTime;
        m_sceneAnimationTimeValid = true;
        return m_sceneAnimationTime;
    }

    const float dt = std::clamp(
        frameTime - m_lastSceneAnimationUpdateTime,
        0.0f,
        kSceneAnimationMaxDeltaSeconds);

    m_lastSceneAnimationUpdateTime = frameTime;

    if (ShouldAdvanceSceneAnimation())
    {
        m_sceneAnimationTime += dt;
    }

    return m_sceneAnimationTime;
}


void Renderer::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat, uint32_t frameCount)
{
    m_backbufferFormat = backbufferFormat;
    
    ConfigureDefaultPointLights();

    // Normal per-frame uploads: constant buffers, small transient data.
    m_upload.Initialize(device, frameCount, 128ull * 1024ull * 1024ull);

    // One-shot/static asset imports: large glTF buffers and texture uploads.
    // This prevents startup asset import pressure from permanently bloating the
    // dynamic frame upload budget.
    m_assetUpload.Initialize(device, frameCount, 512ull * 1024ull * 1024ull);

    // CPU-only DSV heap
    m_dsvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16, false, L"DSV Heap (CPU)");
    
    // Shader-visible SRV heap for textures
    m_srvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true, L"SRV Heap (Shader Visible)");

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
    m_assetUpload.BeginFrame(frameIndex);
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

    const float sceneTime = UpdateSceneAnimationTime(time);

    BuildDrawList(sceneTime);

    BuildRtDrawItems();

    if (!IsDebugViewSelectable(m_debugView))
    {
        m_debugView = 0;
    }

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
    const bool wantsRtSamplingDebug = rtDebug.wantsRtSamplingDebug;
    const bool wantsRtRestirDebug = rtDebug.wantsRtRestirDebug;

    RtPostMode rtPostMode = ResolveRtPostMode(rtDebug);

    const bool allowsStationaryCameraAccumulation =
        m_cameraMode == CameraControlMode::ManualOrbit ||
        m_cameraMode == CameraControlMode::FreeRoam;

    const bool allowRtAccumulation =
        m_rtAccumulate &&
        !wantsMotionDilateDebug &&
        !wantsViewZReconstructDebug &&
        !wantsDiffuseDemodDebug &&
        !wantsOutlierClampDebug &&
        !wantsRtSamplingDebug &&      
        !wantsRtRestirDebug &&
        ((m_debugView == 0) || wantsRtInspectionDebug) &&
        (
            m_rtTemporal ||
            allowsStationaryCameraAccumulation ||
            wantsSplitDebug ||
            wantsMotionDebug ||
            wantsViewZDebug ||
            wantsSurfaceIdDebug ||
            wantsDiffuseAlbedoDebug ||
            wantsRtSamplingDebug
        );

    bool drawListStructuralChanged =
        !m_rtHistoryValid ||
        (m_draws.size() != m_prevRtWorlds.size()) ||
        (m_draws.size() != m_prevRtMeshes.size()) ||
        (m_draws.size() != m_prevRtMaterials.size()) ||
        (m_draws.size() != m_prevRtSubmeshIndices.size());

    bool drawTransformChanged = !m_rtHistoryValid;

    if (!drawListStructuralChanged)
    {
        for (size_t i = 0; i < m_draws.size(); ++i)
        {
            if (m_prevRtMeshes[i] != m_draws[i].mesh ||
                m_prevRtMaterials[i] != m_draws[i].material ||
                m_prevRtSubmeshIndices[i] != m_draws[i].submeshIndex ||
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

    
    const bool cameraRevisionChanged =
        (m_cameraRevision != m_prevRtCameraRevision);

    const bool bigCameraChanged =
        !m_rtHistoryValid ||
        cameraRevisionChanged ||
        (std::fabs(m_camYaw - m_prevRtCamYaw) > 0.05f) ||
        (std::fabs(m_camPitch - m_prevRtCamPitch) > 0.05f) ||
        (std::fabs(m_camRadius - m_prevRtCamRadius) > 0.10f);

    const bool cameraChanged =
        !m_rtHistoryValid ||
        cameraRevisionChanged ||
        (std::fabs(m_camYaw - m_prevRtCamYaw) > 1e-6f) ||
        (std::fabs(m_camPitch - m_prevRtCamPitch) > 1e-6f) ||
        (std::fabs(m_camRadius - m_prevRtCamRadius) > 1e-6f);

    const bool debugViewChanged =
        !m_rtHistoryValid ||
        (m_debugView != m_prevRtDebugView);

    const bool accumulationModeChanged =
        (allowRtAccumulation != m_rtAccumulatingLastFrame);

    const bool envIntegratorChanged =
        (m_rtEnvSamplingMode != m_prevRtEnvSamplingMode) ||
        (m_rtUseEnvImportanceSampling != m_prevRtUseEnvImportanceSampling) ||
        (m_rtUseEnvMIS != m_prevRtUseEnvMIS) ||
        (std::fabs(m_rtEnvMISPower - m_prevRtEnvMISPower) > 1e-6f) ||
        (std::fabs(m_rtEnvIntensity - m_prevRtEnvIntensity) > 1e-6f) ||
        (std::fabs(m_rtEnvPdfEpsilon - m_prevRtEnvPdfEpsilon) > 1e-9f) ||
        (m_rtUseEnvNeeForFinal != m_prevRtUseEnvNeeForFinal) ||
        (m_rtEnvNeeFireflyGuard != m_prevRtEnvNeeFireflyGuard) ||
        (std::fabs(m_rtEnvNeeMaxRadiance - m_prevRtEnvNeeMaxRadiance) > 1e-6f) ||
        (m_rtEnvAliasVersion != m_prevRtEnvAliasVersion) ||
        (std::fabs(m_rtEnvDeltaRoughnessCutoff - m_prevRtEnvDeltaRoughnessCutoff) > 1e-6f);

    const bool integratorChanged =
        !m_rtHistoryValid ||
        (m_rtEnableIndirect != m_prevRtEnableIndirect) ||
        (std::fabs(m_rtIndirectScale - m_prevRtIndirectScale) > 1e-6f) ||
        envIntegratorChanged;

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

    const bool restirHistorySettingsChanged =
        (m_rtEnableRestirEnvDi != m_prevRtEnableRestirEnvDi) ||
        (m_rtRestirUseTemporal != m_prevRtRestirUseTemporal) ||
        (m_rtRestirUseSpatial != m_prevRtRestirUseSpatial) ||
        (m_rtRestirInitialCandidateCount != m_prevRtRestirInitialCandidateCount) ||
        (m_rtRestirSpatialSamples != m_prevRtRestirSpatialSamples) ||
        (m_rtRestirSpatialRadius != m_prevRtRestirSpatialRadius) ||
        (std::fabs(m_rtRestirNormalSigma - m_prevRtRestirNormalSigma) > 1e-6f) ||
        (std::fabs(m_rtRestirDepthSigma - m_prevRtRestirDepthSigma) > 1e-6f) ||
        (std::fabs(m_rtRestirViewZSigma - m_prevRtRestirViewZSigma) > 1e-6f) ||
        (std::fabs(m_rtRestirRoughnessSigma - m_prevRtRestirRoughnessSigma) > 1e-6f) ||
        (std::fabs(m_rtRestirMaxM - m_prevRtRestirMaxM) > 1e-6f) ||
        (std::fabs(m_rtRestirMaxAge - m_prevRtRestirMaxAge) > 1e-6f) ||
        (std::fabs(m_rtRestirMinTarget - m_prevRtRestirMinTarget) > 1e-9f) ||
        (std::fabs(m_rtRestirMaxWeight - m_prevRtRestirMaxWeight) > 1e-6f) ||
        (std::fabs(m_rtRestirTemporalMinConfidence - m_prevRtRestirTemporalMinConfidence) > 1e-6f);

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

    if (resetTemporalHistory ||
        restirHistorySettingsChanged ||
        viewZReconsSettingsChanged ||
        viewZPolicyChanged)
    {
        ResetRtRestirHistory();
    }

    // This must always be assigned before DXR dispatch and before post-stack gating.
    // RayGen receives this through PerFrameConstants::rtAccumulate.
    m_rtAccumulateThisFrame = allowRtAccumulation;

    if (wantsMotionDilateDebug ||
        wantsViewZReconstructDebug ||
        wantsDiffuseDemodDebug ||
        wantsOutlierClampDebug ||
        wantsRtSamplingDebug ||
        wantsRtRestirDebug)
    {
        // Debug 54 is produced by RtMotionDilatePass.
        // DXR still runs to author raw motion/depth/normal guides, but it must not
        // advance progressive diffuse/spec accumulation while the compute debug
        // pass owns the final m_rtOutput image.
        m_rtAccumulateThisFrame = false;
        m_rtDispatchSampleIndex = 0;
    }

    // ReSTIR temporal/spatial/resolve debug views still need guide histories to
    // advance even when the normal RT post stack is disabled. Debug display
    // ownership must not invalidate the histories required to validate reuse.
    const bool restirNeedsPersistentGuideHistory =
        m_rtEnableRestirEnvDi ||
        rtDebug.wantsRtRestirTemporalDebug ||
        rtDebug.wantsRtRestirSpatialDebug ||
        rtDebug.wantsRtRestirResolveDebug;

    if ((rtPostMode == RtPostMode::Disabled ||
        rtPostMode == RtPostMode::RawCombineOnly) &&
        !restirNeedsPersistentGuideHistory)
    {
        m_rtTemporalHistoryValid = false;
        m_rtSurfaceIdHistoryValid = false;
    }

    m_prevRtCamYaw = m_camYaw;
    m_prevRtCamPitch = m_camPitch;
    m_prevRtCamRadius = m_camRadius;
    m_prevRtCameraRevision = m_cameraRevision;
    m_prevRtDebugView = m_debugView;
    m_prevRtHasBrdfLut = rtHasBrdfLut;
    m_prevRtHasIbl = rtHasIbl;
    m_rtAccumulatingLastFrame = m_rtAccumulateThisFrame;

    m_prevRtWorlds.resize(m_draws.size());
    m_prevRtMeshes.resize(m_draws.size());
    m_prevRtMaterials.resize(m_draws.size());
    m_prevRtSubmeshIndices.resize(m_draws.size());
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
    m_prevRtUseEnvNeeForFinal = m_rtUseEnvNeeForFinal;
    m_prevRtEnvNeeFireflyGuard = m_rtEnvNeeFireflyGuard;
    m_prevRtEnvNeeMaxRadiance = m_rtEnvNeeMaxRadiance;
    m_prevRtEnvAliasVersion = m_rtEnvAliasVersion;

    for (size_t i = 0; i < m_draws.size(); ++i)
    {
        m_prevRtWorlds[i] = m_draws[i].world;
        m_prevRtMeshes[i] = m_draws[i].mesh;
        m_prevRtMaterials[i] = m_draws[i].material;
        m_prevRtSubmeshIndices[i] = m_draws[i].submeshIndex;
    }
    m_prevRtEnableIndirect = m_rtEnableIndirect;
    m_prevRtIndirectScale = m_rtIndirectScale;

    m_prevRtEnvSamplingMode = m_rtEnvSamplingMode;
    m_prevRtUseEnvImportanceSampling = m_rtUseEnvImportanceSampling;
    m_prevRtUseEnvMIS = m_rtUseEnvMIS;
    m_prevRtEnvMISPower = m_rtEnvMISPower;
    m_prevRtEnvIntensity = m_rtEnvIntensity;
    m_prevRtEnvPdfEpsilon = m_rtEnvPdfEpsilon;
    m_prevRtEnvDeltaRoughnessCutoff = m_rtEnvDeltaRoughnessCutoff;

    m_prevRtEnableRestirEnvDi = m_rtEnableRestirEnvDi;
    m_prevRtRestirUseTemporal = m_rtRestirUseTemporal;
    m_prevRtRestirUseSpatial = m_rtRestirUseSpatial;

    m_prevRtRestirInitialCandidateCount = m_rtRestirInitialCandidateCount;
    m_prevRtRestirSpatialSamples = m_rtRestirSpatialSamples;
    m_prevRtRestirSpatialRadius = m_rtRestirSpatialRadius;

    m_prevRtRestirNormalSigma = m_rtRestirNormalSigma;
    m_prevRtRestirDepthSigma = m_rtRestirDepthSigma;
    m_prevRtRestirViewZSigma = m_rtRestirViewZSigma;
    m_prevRtRestirRoughnessSigma = m_rtRestirRoughnessSigma;

    m_prevRtRestirMaxM = m_rtRestirMaxM;
    m_prevRtRestirMaxAge = m_rtRestirMaxAge;
    m_prevRtRestirMinTarget = m_rtRestirMinTarget;
    m_prevRtRestirMaxWeight = m_rtRestirMaxWeight;
    m_prevRtRestirTemporalMinConfidence = m_rtRestirTemporalMinConfidence;

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

        //reset per-frame execution results
        m_rtRestirTemporalValidThisFrame = false;
        m_rtRestirSpatialValidThisFrame = false;
        m_rtRestirResolvedValidThisFrame = false;

        EnsureRtOutputSize(width, height);
        BuildRtDrawItems();
        BuildRtMaterialTable();
        UpdateRtSceneStats();
        LogRtSceneStatsIfChanged();

        const bool rtSceneValid =
            ValidateRtSceneContract();

        if (rtSceneValid)
        {
            EnsureRtInstanceData(frameIndex);
            EnsureRtEnvironmentAlias(device, cl);
            EnsureRtRestirResources(width, height);
            UpdateRtGeometryTable(frameIndex);

            const bool restirNeedsFreshDxr =
                m_rtEnableRestirEnvDi ||
                wantsRtRestirDebug;

            const bool canReuseAccumulatedOutput =
                !restirNeedsFreshDxr &&
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

                    const bool usePreviewSampleBatch =
                        !m_rtAccumulateThisFrame &&
                        m_debugView == 0 &&
                        m_rtEnableIndirect != 0 &&
                        m_rtPreviewSamplesPerFrame > 1u;

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
                    }
                    else if (usePreviewSampleBatch)
                    {
                        dispatchCount =
                            std::min<uint32_t>(
                                std::max<uint32_t>(m_rtPreviewSamplesPerFrame, 1u),
                                4u);
                    }

                    for (uint32_t i = 0; i < dispatchCount; ++i)
                    {
                        m_rtDispatchUsesAccumulationPath =
                            m_rtAccumulateThisFrame || usePreviewSampleBatch;

                        m_rtDispatchSampleIndex =
                            m_rtAccumulateThisFrame
                            ? (m_rtSampleIndex + i)
                            : (usePreviewSampleBatch ? i : 0u);

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

                    if (m_rtRestirResourcesReady &&
                        m_rtRestirInitialReservoir &&
                        (m_rtEnableRestirEnvDi || wantsRtRestirDebug))
                    {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        b.UAV.pResource = m_rtRestirInitialReservoir.Get();

                        cmd4->ResourceBarrier(1, &b);
                    }
                }
            }

            RunRtDenoiser(
                cl,
                frameIndex,
                device,
                width,
                height);

            const bool ranRestirResolve =
                RunRtRestirResolve(
                    cl,
                    frameIndex,
                    device,
                    width,
                    height,
                    sceneTime,
                    rtDebug);

            if (ranRestirResolve)
            {
                RunRtRestirApplyBeauty(
                    cl,
                    frameIndex,
                    device,
                    width,
                    height,
                    rtDebug);
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

            if (m_rtRestirResourcesReady)
            {
                cl.Transition(m_rtRestirInitialReservoir.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cl.Transition(m_rtRestirTemporalReservoir[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cl.Transition(m_rtRestirTemporalReservoir[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cl.Transition(m_rtRestirSpatialReservoir.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cl.Transition(m_rtRestirResolvedDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cl.Transition(m_rtRestirResolvedSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (m_rtRestirAppliedReady)
                {
                    cl.Transition(m_rtRestirAppliedDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cl.Transition(m_rtRestirAppliedSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
            m_rtDispatchUsesAccumulationPath = false;
            CmdEndEvent(cmdList); //DXR
        }

        else
        {
            m_rtDispatchUsesAccumulationPath = false;
            OutputDebugStringW(
                L"DXR scene contract invalid this frame; skipping DXR dispatch.\n");
            CmdEndEvent(cmdList); //DXR
        }
    }
    else
    {
        OutputDebugStringW(
            L"DXR unavailable or disabled this frame; using raster path.\n");
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
                if (!item.mesh)
                    continue;

                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                const Mesh::Submesh& submesh =
                    item.mesh->GetSubmesh(item.submeshIndex);
                
                m_shadowPass.Render(cl, m_shadowSize, perFrameCb, perDrawCb, *item.mesh, &submesh);
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
                if (!item.mesh || !item.material)
                    continue;

                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                const Mesh::Submesh& submesh =
                    item.mesh->GetSubmesh(item.submeshIndex);
                m_gbufferPass.Render(
                    cl,
                    width,
                    height,
                    perFrameCb,
                    perDrawCb,
                    m_scene.table.gpu,
                    *item.material,
                    *item.mesh,
                    &submesh);
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
                if (!item.mesh || !item.material)
                    continue;

                const D3D12_GPU_VIRTUAL_ADDRESS perDrawCb = UploadPerDrawConstants(frameIndex, item);
                const Mesh::Submesh& submesh =
                    item.mesh->GetSubmesh(item.submeshIndex);
                m_forwardPbr.Render(
                    cl,
                    width,
                    height,
                    perFrameCb,
                    perDrawCb,
                    m_scene.table.gpu,
                    *item.material,
                    *item.mesh,
                    &submesh);
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
    
    ResetRtRestirResources();

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
        EnsureRtRestirResources(width, height);

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
    if (m_cameraMode == CameraControlMode::AutoOrbit)
    {
        if (m_cameraTimeValid)
        {
            const float dt = std::clamp(
                time - m_lastCameraUpdateTime,
                0.0f,
                kCameraMaxDeltaSeconds);

            m_camYaw = WrapAngleRadians(
                m_camYaw + dt * kOrbitAutoYawSpeed);
        }

        m_lastCameraUpdateTime = time;
        m_cameraTimeValid = true;
    }
    else
    {
        // Keep the time baseline current while manual/free camera control is active
        // so re-enabling auto-orbit can resume from the current yaw without a large
        // first-frame delta.
        m_lastCameraUpdateTime = time;
        m_cameraTimeValid = true;
    }

    if (m_cameraMode == CameraControlMode::FreeRoam)
    {
        using namespace DirectX;

        const XMVECTOR position =
            XMLoadFloat3(&m_freeCamPosition);

        const XMVECTOR target =
            position + FreeRoamForwardVector(m_freeCamYaw, m_freeCamPitch);

        XMStoreFloat3(&m_sceneData.camera.position, position);
        XMStoreFloat3(&m_sceneData.camera.target, target);
    }
    else
    {
        ComputeOrbitCamera(
            m_sceneData.camera.position,
            m_sceneData.camera.target);
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
    cb->rtAccumulate = m_rtDispatchUsesAccumulationPath ? 1u : 0u;
    cb->rtEnableIndirect = m_rtEnableIndirect ? 1u : 0u;
    cb->rtIndirectScale = m_rtIndirectScale;
    
    m_currViewProj = cb->viewProj;
    m_currInvViewProj = cb->invViewProj;

    cb->pointLightCount =
        std::min<uint32_t>(m_sceneData.pointLightCount, kMaxPointLights);

    cb->pointLightPad = { 0.0f, 0.0f, 0.0f };

    for (uint32_t i = 0; i < kMaxPointLights; ++i)
    {
        cb->pointLights[i] =
            (i < cb->pointLightCount)
            ? m_sceneData.pointLights[i]
            : PointLight{};
    }

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
        EnsureRtRestirResources(m_widthCached, m_heightCached);
        m_rtAtrousPass.Initialize(device, Paths::ShaderDir());
        m_rtHistorySelectPass.Initialize(device, Paths::ShaderDir());
        m_rtCombinePass.Initialize(device, Paths::ShaderDir());
        m_rtViewZReconstructPass.Initialize(device, Paths::ShaderDir());
        m_rtDiffuseDemodulatePass.Initialize(device, Paths::ShaderDir());
        m_rtOutlierClampPass.Initialize(device, Paths::ShaderDir());
        m_rtRestirTemporalPass.Initialize(device, Paths::ShaderDir());
        m_rtRestirSpatialPass.Initialize(device, Paths::ShaderDir());
        m_rtRestirApplyPass.Initialize(device, Paths::ShaderDir());
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

        const std::filesystem::path iblDiffusePath =
            content / L"Textures" / L"lilienstein_2kblurred.png";

        if (std::filesystem::exists(iblDiffusePath))
        {
            m_iblDiffuseTex.LoadFromFile_DirectXTex(
                device, cl, m_upload, frameIndex,
                iblDiffusePath,
                false,
                L"Tex: IBL Diffuse");
        }
        else
        {
            DebugOutput("Warning: lilienstein_2kblurred.png missing. Using null scene slot.");
        }

        const std::filesystem::path envRadiancePath =
            content / L"Textures" / L"lilienstein_2k.png";

        if (std::filesystem::exists(envRadiancePath))
        {
            m_iblSpecularTex.LoadFromFile_DirectXTex(
                device, cl, m_upload, frameIndex,
                envRadiancePath,
                false,
                L"Tex: IBL Specular");

            if (m_dxrAvailable && m_device5)
            {
                LoadRtEnvironmentCpuRadiance(envRadiancePath);
            }
        }
        else
        {
            DebugOutput("Warning: lilienstein_2k.png missing. Using null scene slot.");

            if (m_dxrAvailable && m_device5)
            {
                ClearRtEnvironmentCpuRadiance();
            }
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

    LoadDefaultGltfScene(device, cl, frameIndex);

    if (m_dxrAvailable &&
        m_device5 &&
        m_importedModel.IsLoaded())
    {
        BuildImportedModelBlas(device, cl);
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
        item.submeshIndex = 0;
        XMStoreFloat4x4(&item.world, XMMatrixIdentity());
        item.rtObjectId = 1u;
        m_draws.push_back(item);
    }

    // Center rotating metal test object
    {
        DrawItem item{};
        item.mesh = &m_quad;
        item.material = &m_metalMaterial;
        item.submeshIndex = 0;
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
        item.submeshIndex = 0;
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
        item.submeshIndex = 0;
        XMMATRIX world =
            XMMatrixScaling(0.9f, 0.9f, 0.9f) *
            XMMatrixTranslation(1.75f, 0.5f, 0.0f);
        XMStoreFloat4x4(&item.world, world);
        item.rtObjectId = 4u;
        m_draws.push_back(item);
    }

    AppendLoadedModelDraws();
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UploadPerDrawConstants(
    uint32_t frameIndex,
    const DrawItem& item)
{
    auto alloc = m_upload.Allocate(frameIndex, sizeof(PerDrawConstants), 256);
    auto* dc = reinterpret_cast<PerDrawConstants*>(alloc.cpu);
    *dc = {};

    dc->world = item.world;
    dc->materialIndex = 0;

    const Material* material = item.material;

    dc->baseColorFactor =
        material ? material->baseColorFactor : DirectX::XMFLOAT4(1, 1, 1, 1);

    dc->metallicFactor =
        material ? material->metallicFactor : 0.0f;

    dc->roughnessFactor =
        material ? material->roughnessFactor : 0.5f;

    dc->occlusionStrength =
        material ? material->occlusionStrength : 1.0f;

    dc->hasOcclusionTexture =
        material ? material->hasOcclusionTexture : 0u;

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
void Renderer::BuildTlasForDrawList(
    uint32_t frameIndex,
    ID3D12GraphicsCommandList4* cmd4)
{
    if (!cmd4)
        return;

    auto& frame =
        m_rtFrames[frameIndex];

    const uint32_t rtInstanceCount =
        static_cast<uint32_t>(m_rtDrawItems.size());

    if (rtInstanceCount == 0)
        return;

    std::vector<AccelerationStructure::InstanceDesc> instances;
    instances.reserve(rtInstanceCount);

    for (uint32_t rtInstanceIndex = 0;
        rtInstanceIndex < rtInstanceCount;
        ++rtInstanceIndex)
    {
        const DrawItem* drawItem =
            m_rtDrawItems[rtInstanceIndex];

        if (!drawItem)
            continue;

        const AccelerationStructure* blas =
            GetBlasForDrawItem(*drawItem);

        if (!blas)
        {
            OutputDebugStringW(
                L"BuildTlasForDrawList skipped RT draw with no BLAS.\n");

            continue;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS blasAddress =
            blas->GpuAddress();

        if (blasAddress == 0)
        {
            OutputDebugStringW(
                L"BuildTlasForDrawList skipped RT draw with invalid BLAS address.\n");

            continue;
        }

        AccelerationStructure::InstanceDesc inst{};

        // This is the critical contract:
        // InstanceID() in HLSL indexes g_InstanceData.
        // EnsureRtInstanceData() writes g_InstanceData in m_rtDrawItems order.
        inst.instanceID = rtInstanceIndex;

        inst.hitGroupIndex = 0;
        inst.mask = 0xFF;
        inst.blasAddress = blasAddress;

        const DirectX::XMFLOAT4X4& world =
            drawItem->world;

        // Preserve your existing transform packing convention.
        // Your current code stores a 3x4 DXR instance transform as:
        //
        // [ _11 _12 _13 _41 ]
        // [ _21 _22 _23 _42 ]
        // [ _31 _32 _33 _43 ]
        //
        // Do not change this unless your AccelerationStructure wrapper
        // explicitly expects the transposed convention.
        inst.transform[0] = world._11;
        inst.transform[1] = world._12;
        inst.transform[2] = world._13;
        inst.transform[3] = world._41;

        inst.transform[4] = world._21;
        inst.transform[5] = world._22;
        inst.transform[6] = world._23;
        inst.transform[7] = world._42;

        inst.transform[8] = world._31;
        inst.transform[9] = world._32;
        inst.transform[10] = world._33;
        inst.transform[11] = world._43;

        instances.push_back(inst);
    }

    if (instances.empty())
        return;

    frame.tlas.BuildTopLevel(
        m_device5.Get(),
        cmd4,
        instances.data(),
        static_cast<uint32_t>(instances.size()),
        L"TLAS Scene");
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
    EnsureRtRestirResources(width, height);
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

    const uint32_t instanceCount =
        static_cast<uint32_t>(m_rtDrawItems.size());

    // No valid DXR instances this frame.
    // The caller should also avoid building/dispatching DXR work with an empty TLAS.
    if (instanceCount == 0)
        return;

    const uint32_t requiredCount =
        std::max<uint32_t>(1u, instanceCount);

    if (!frame.instanceDataUpload || frame.capacity < requiredCount)
    {
        const uint32_t newCapacity =
            std::max<uint32_t>(
                requiredCount,
                frame.capacity ? frame.capacity * 2u : 8u);

        frame.capacity = newCapacity;

        const uint64_t bufferSize =
            uint64_t(frame.capacity) * uint64_t(sizeof(RTInstanceData));

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        ThrowIfFailed(
            m_device5->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&frame.instanceDataUpload)),
            "Create RT instance data upload");

        SetD3D12ObjectName(
            frame.instanceDataUpload.Get(),
            L"RT Instance Data Upload");

        if (!frame.instanceDataSrv.IsValid())
        {
            frame.instanceDataSrv =
                m_srvHeap.Allocate(1);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = frame.capacity;
        srv.Buffer.StructureByteStride = sizeof(RTInstanceData);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        m_device5->CreateShaderResourceView(
            frame.instanceDataUpload.Get(),
            &srv,
            frame.instanceDataSrv.cpu);
    }

    assert(instanceCount <= frame.capacity);

    D3D12_RANGE readRange{ 0, 0 };

    RTInstanceData* dst = nullptr;

    ThrowIfFailed(
        frame.instanceDataUpload->Map(
            0,
            &readRange,
            reinterpret_cast<void**>(&dst)),
        "Map RT instance data upload");

    for (uint32_t instanceIndex = 0;
        instanceIndex < instanceCount;
        ++instanceIndex)
    {
        const DrawItem& item =
            *m_rtDrawItems[instanceIndex];

        const Material* material =
            item.material;

        RTInstanceData data{};

        data.baseColorFactor =
            material
            ? material->baseColorFactor
            : DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

        data.metallic =
            material
            ? material->metallicFactor
            : 0.0f;

        data.roughness =
            material
            ? material->roughnessFactor
            : 0.5f;

        data.occlusionStrength =
            material
            ? material->occlusionStrength
            : 1.0f;

        data.hasOcclusionTexture =
            material
            ? material->hasOcclusionTexture
            : 0u;

        data.meshType =
            GetRtMeshTypeForDrawItem(item);

        data.materialId =
            ResolveRtMaterialId(material);

        data.objectId =
            item.rtObjectId != 0u
            ? item.rtObjectId
            : instanceIndex + 1u;

        data.indexStart =
            GetRtIndexStartForDrawItem(item);

        const bool hasPrevMotionWorld =
            m_prevRtMotionWorldsValid &&
            instanceIndex < m_prevRtMotionWorlds.size();

        data.prevObjectToWorld =
            hasPrevMotionWorld
            ? m_prevRtMotionWorlds[instanceIndex]
            : item.world;

        dst[instanceIndex] = data;
    }

    const D3D12_RANGE writtenRange
    {
        0,
        SIZE_T(sizeof(RTInstanceData) * instanceCount)
    };

    frame.instanceDataUpload->Unmap(
        0,
        &writtenRange);
}

void Renderer::UpdateRtGeometryTable(
    uint32_t frameIndex,
    ID3D12Resource* restirResolveReservoir)
{
    auto& frame =
        m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.geometryTable,
        frame.geometryTableCount,
        kRtSrvTableCount);

    auto CpuForRegister = [&](uint32_t shaderRegister)
    {
        assert(shaderRegister >= 1u);
        assert(shaderRegister <= kRtHighestSrvRegister);

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            frame.geometryTable.cpu;

        handle.ptr +=
            SIZE_T(shaderRegister - 1u) *
            SIZE_T(m_srvHeap.DescriptorSize());

        return handle;
    };

    auto CreateNullStructuredBufferSrv =
        [&](uint32_t shaderRegister, uint32_t stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension =
            D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Format =
            DXGI_FORMAT_UNKNOWN;
        desc.Buffer.FirstElement =
            0;
        desc.Buffer.NumElements =
            1;
        desc.Buffer.StructureByteStride =
            stride;
        desc.Buffer.Flags =
            D3D12_BUFFER_SRV_FLAG_NONE;

        m_device5->CreateShaderResourceView(
            nullptr,
            &desc,
            CpuForRegister(shaderRegister));
    };

    auto CreateNullRawBufferSrv =
        [&](uint32_t shaderRegister)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension =
            D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Format =
            DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.FirstElement =
            0;
        desc.Buffer.NumElements =
            1;
        desc.Buffer.StructureByteStride =
            0;
        desc.Buffer.Flags =
            D3D12_BUFFER_SRV_FLAG_RAW;

        m_device5->CreateShaderResourceView(
            nullptr,
            &desc,
            CpuForRegister(shaderRegister));
    };

    auto CreateStructuredBufferSrv =
        [&](ID3D12Resource* resource,
            uint32_t shaderRegister,
            uint32_t elementCount,
            uint32_t stride)
    {
        if (!resource || elementCount == 0u)
        {
            CreateNullStructuredBufferSrv(
                shaderRegister,
                stride);

            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension =
            D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Format =
            DXGI_FORMAT_UNKNOWN;
        desc.Buffer.FirstElement =
            0;
        desc.Buffer.NumElements =
            elementCount;
        desc.Buffer.StructureByteStride =
            stride;
        desc.Buffer.Flags =
            D3D12_BUFFER_SRV_FLAG_NONE;

        m_device5->CreateShaderResourceView(
            resource,
            &desc,
            CpuForRegister(shaderRegister));
    };

    auto CreateRawBufferSrv =
        [&](ID3D12Resource* resource,
            uint32_t shaderRegister,
            uint32_t byteSize)
    {
        if (!resource || byteSize == 0u)
        {
            CreateNullRawBufferSrv(shaderRegister);
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension =
            D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Format =
            DXGI_FORMAT_R32_TYPELESS;
        desc.Buffer.FirstElement =
            0;
        desc.Buffer.NumElements =
            (byteSize + 3u) / 4u;
        desc.Buffer.StructureByteStride =
            0;
        desc.Buffer.Flags =
            D3D12_BUFFER_SRV_FLAG_RAW;

        m_device5->CreateShaderResourceView(
            resource,
            &desc,
            CpuForRegister(shaderRegister));
    };

    auto CreateTextureSrv =
        [&](const Texture* texture,
            uint32_t shaderRegister,
            const Texture* fallback = nullptr)
    {
        const Texture* selected =
            texture && texture->IsValid()
            ? texture
            : fallback;

        if (selected && selected->IsValid())
        {
            WriteRtTextureSrv(
                m_device5.Get(),
                CpuForRegister(shaderRegister),
                *selected);

            return;
        }

        CreateNullTexture2DSRV(
            m_device5.Get(),
            CpuForRegister(shaderRegister),
            DXGI_FORMAT_R8G8B8A8_UNORM);
    };

    // Fill the whole table with deterministic null texture descriptors first.
    // Explicit buffer slots are overwritten below with proper buffer SRVs.
    for (uint32_t shaderRegister = 1u;
        shaderRegister <= kRtHighestSrvRegister;
        ++shaderRegister)
    {
        CreateNullTexture2DSRV(
            m_device5.Get(),
            CpuForRegister(shaderRegister),
            DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    // t1 = quad vertices
    CreateStructuredBufferSrv(
        m_quad.VertexBufferResource(),
        kRtRegisterQuadVerts,
        m_quad.VertexCount(),
        m_quad.VertexStride());

    // t2 = quad indices, 16-bit procedural index buffer exposed as raw.
    CreateRawBufferSrv(
        m_quad.IndexBufferResource(),
        kRtRegisterQuadIndices,
        static_cast<uint32_t>(m_quad.IndexCount() * sizeof(uint16_t)));

    // t3 = floor vertices
    CreateStructuredBufferSrv(
        m_floor.VertexBufferResource(),
        kRtRegisterFloorVerts,
        m_floor.VertexCount(),
        m_floor.VertexStride());

    // t4 = floor indices, 16-bit procedural index buffer exposed as raw.
    CreateRawBufferSrv(
        m_floor.IndexBufferResource(),
        kRtRegisterFloorIndices,
        static_cast<uint32_t>(m_floor.IndexCount() * sizeof(uint16_t)));

    // t5 = RT instance data.
    //
    // Important: this count must match the same RT draw list used for TLAS.
    CreateStructuredBufferSrv(
        frame.instanceDataUpload.Get(),
        kRtRegisterInstanceData,
        static_cast<uint32_t>(m_rtDrawItems.size()),
        sizeof(RTInstanceData));

    // t6..t261 = material texture table.
    //
    // 64 materials × 4 textures:
    // base color, normal, metallic/roughness, occlusion.
    for (uint32_t materialIndex = 0u;
        materialIndex < kRtMaxMaterials;
        ++materialIndex)
    {
        const Material* material =
            materialIndex < m_rtMaterialTable.size()
            ? m_rtMaterialTable[materialIndex]
            : nullptr;

        const uint32_t baseRegister =
            kRtRegisterMaterialTextures +
            materialIndex * kRtTexturesPerMaterial;

        CreateTextureSrv(
            material ? material->baseColorTexture : nullptr,
            baseRegister + kRtBaseColorTextureSlot,
            &m_rtFallbackWhiteTex);

        CreateTextureSrv(
            material ? material->normalTexture : nullptr,
            baseRegister + kRtNormalTextureSlot,
            &m_rtFallbackFlatNormalTex);

        CreateTextureSrv(
            material ? material->metalRoughTexture : nullptr,
            baseRegister + kRtMetalRoughTextureSlot,
            &m_rtFallbackOrmTex);

        CreateTextureSrv(
            material ? material->occlusionTexture : nullptr,
            baseRegister + kRtOcclusionTextureSlot,
            &m_rtFallbackWhiteTex);
    }

    // t270/t271 = imported glTF combined mesh buffers.
    if (m_importedModel.IsLoaded())
    {
        Mesh& importedMesh =
            m_importedModel.GetMesh();

        CreateStructuredBufferSrv(
            importedMesh.VertexBufferResource(),
            kRtRegisterImportedVerts,
            importedMesh.VertexCount(),
            importedMesh.VertexStride());

        CreateRawBufferSrv(
            importedMesh.IndexBufferResource(),
            kRtRegisterImportedIndices,
            importedMesh.IndexBufferByteSize());
    }
    else
    {
        CreateNullStructuredBufferSrv(
            kRtRegisterImportedVerts,
            sizeof(Mesh::Vertex));

        CreateNullRawBufferSrv(
            kRtRegisterImportedIndices);
    }

    // t280 = BRDF LUT
    CreateTextureSrv(
        m_brdfLutTex.IsValid() ? &m_brdfLutTex : nullptr,
        kRtRegisterBrdfLut);

    // t281 = IBL diffuse
    CreateTextureSrv(
        m_iblDiffuseTex.IsValid() ? &m_iblDiffuseTex : nullptr,
        kRtRegisterIblDiffuse);

    // t282 = IBL specular
    CreateTextureSrv(
        m_iblSpecularTex.IsValid() ? &m_iblSpecularTex : nullptr,
        kRtRegisterIblSpecular);

    // t283 = environment alias table.
    //
    // Write a valid null first, then let the real helper overwrite it if the
    // environment alias resource exists.
    WriteNullRtEnvAliasSrv(
        CpuForRegister(kRtRegisterEnvAlias));

    WriteRtEnvAliasSrv(
        CpuForRegister(kRtRegisterEnvAlias));

    // t284 = selected ReSTIR reservoir for DXR resolve.
    const uint32_t pixelCount =
        std::max(1u, m_widthCached * m_heightCached);

    CreateStructuredBufferSrv(
        restirResolveReservoir,
        kRtRegisterRestirResolveReservoir,
        restirResolveReservoir ? pixelCount : 1u,
        sizeof(RtRestirReservoir));

    m_rtMaterialCount =
        static_cast<uint32_t>(
            std::min<size_t>(
                m_rtMaterialTable.size(),
                kRtMaxMaterials));
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

        ResetRtRestirHistory();
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
    m_prevRtMotionWorlds.clear();

    if (m_rtDrawItems.empty())
    {
        m_prevRtMotionWorldsValid = false;
        return;
    }

    m_prevRtMotionWorlds.reserve(m_rtDrawItems.size());

    for (const DrawItem* item : m_rtDrawItems)
    {
        if (!item)
        {
            m_prevRtMotionWorldsValid = false;
            m_prevRtMotionWorlds.clear();
            return;
        }

        m_prevRtMotionWorlds.push_back(item->world);
    }

    m_prevRtMotionWorldsValid = true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtRayGenConstants(uint32_t frameIndex, uint32_t restirDispatchMode)
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

    const bool envAliasUsable =
        m_rtEnvAliasReady &&
        m_rtEnvAliasBuffer &&
        m_rtEnvAliasCount > 0 &&
        m_rtEnvFaceSize > 0;

    cb->sampling.envSamplingMode =
        static_cast<uint32_t>(m_rtEnvSamplingMode);

    cb->sampling.useEnvImportanceSampling =
        (m_rtUseEnvImportanceSampling && envAliasUsable) ? 1u : 0u;

    cb->sampling.useEnvMIS =
        (m_rtUseEnvMIS && envAliasUsable) ? 1u : 0u;

    cb->sampling.envAliasCount =
        envAliasUsable ? m_rtEnvAliasCount : 0u;

    cb->sampling.envFaceSize =
        envAliasUsable ? m_rtEnvFaceSize : 0u;

    cb->sampling.envAliasFallback =
        (!envAliasUsable || m_rtEnvAliasFallback) ? 1u : 0u;

    cb->sampling.samplingDebugView = m_debugView;

    // Anti-double-counting guard.
    // When resolved ReSTIR environment lighting is applied to beauty, disable
    // the older final environment NEE path so direct environment lighting is not
    // added twice.
    const bool applyingRestirToBeauty =
        m_rtEnableRestirEnvDi &&
        m_rtRestirResolveToBeauty &&
        m_rtRestirApplyMode != 0u;

    cb->sampling.useEnvNeeForFinal =
        (m_rtUseEnvNeeForFinal &&
            !applyingRestirToBeauty &&
            envAliasUsable &&
            !m_rtEnvAliasFallback)
        ? 1u
        : 0u;

    cb->sampling.envIntensity = m_rtEnvIntensity;
    cb->sampling.envPdfEpsilon = m_rtEnvPdfEpsilon;
    cb->sampling.envDeltaRoughnessCutoff = m_rtEnvDeltaRoughnessCutoff;
    cb->sampling.envMISPower = m_rtEnvMISPower;

    cb->sampling.envNeeFireflyGuard =
        m_rtEnvNeeFireflyGuard ? 1u : 0u;

    cb->sampling.envAliasVersion =
        m_rtEnvAliasVersion;

    cb->sampling.envNeeMaxRadiance =
        std::max(1e-4f, m_rtEnvNeeMaxRadiance);

    cb->sampling.pad1 = 0.0f;

    cb->restir.enableRestirEnvDi =
        (m_rtEnableRestirEnvDi || IsRtRestirDebug(m_debugView)) ? 1u : 0u;

    cb->restir.restirInitialCandidateCount =
        std::max(1u, m_rtRestirInitialCandidateCount);

    cb->restir.restirDebugView =
        m_debugView;

    cb->restir.restirMaxM =
        std::max(1.0f, m_rtRestirMaxM);

    cb->restir.restirMaxAge =
        std::max(0.0f, m_rtRestirMaxAge);

    cb->restir.restirMinTarget =
        std::max(0.0f, m_rtRestirMinTarget);

    cb->restir.restirMaxWeight =
        std::max(1.0f, m_rtRestirMaxWeight);

    cb->restir.restirDispatchMode = restirDispatchMode;

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
    r.wantsRtSamplingDebug = IsRtSamplingDebug(debugView);
    r.wantsRtRestirRayGenDebug = IsRtRestirRayGenDebug(debugView);
    r.wantsRtRestirTemporalDebug = IsRtRestirTemporalDebug(debugView);
    r.wantsRtRestirSpatialDebug = IsRtRestirSpatialDebug(debugView);
    r.wantsRtRestirResolveDebug = IsRtRestirResolveDebug(debugView);
    r.wantsRtRestirDebug = IsRtRestirDebug(debugView);

    r.wantsSpatialDebug =
        r.wantsSvgfDebug ||
        r.wantsRtRestirSpatialDebug;

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
        r.wantsOutlierClampDebug ||
        r.wantsRtSamplingDebug ||
        r.wantsRtRestirDebug;

    r.wantsRtInspectionDebug =
        r.wantsRtPostDebug ||
        r.wantsProducerDebug;

    if (r.wantsSplitDebug ||
        r.wantsMotionDebug ||
        r.wantsViewZDebug ||
        r.wantsSurfaceIdDebug ||
        r.wantsDiffuseAlbedoDebug ||
        r.wantsRtSamplingDebug ||
        r.wantsRtRestirRayGenDebug)
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
    else if (r.wantsTemporalDebug ||
        r.wantsRtRestirTemporalDebug)
    {
        r.owner = RtDebugOwner::Temporal;
    }
    else if (r.wantsHistorySelectDebug)
    {
        r.owner = RtDebugOwner::HistorySelect;
    }
    else if (r.wantsSvgfDebug ||
        r.wantsRtRestirSpatialDebug)
    {
        r.owner = RtDebugOwner::Spatial;
    }
    else if (r.wantsRtRestirResolveDebug)
    {
        r.owner = RtDebugOwner::Combine;
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

    const bool restirNeedsTemporalGuides =
        m_rtEnableRestirEnvDi ||
        rtDebug.wantsRtRestirTemporalDebug ||
        rtDebug.wantsRtRestirSpatialDebug ||
        rtDebug.wantsRtRestirResolveDebug;

    const bool shouldRunViewZReconstruct =
        temporalWouldRun ||
        postMayRunSpecSpatial ||
        restirNeedsTemporalGuides ||
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
            restirNeedsTemporalGuides ||
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

    const bool ranRestirTemporal =
        RunRtRestirTemporal(
            cl,
            frameIndex,
            device,
            width,
            height,
            guides,
            ranViewZReconstruct,
            ranMotionDilate,
            rtDebug);

    const bool ranRestirSpatial =
        RunRtRestirSpatial(
            cl,
            frameIndex,
            device,
            width,
            height,
            guides,
            ranViewZReconstruct,
            ranRestirTemporal,
            rtDebug);

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

    const bool restirCanCommitGuideHistory =
        restirNeedsTemporalGuides &&
        guides.normalRough &&
        guides.depth &&
        guides.surfaceId &&
        ranViewZReconstruct &&
        ranMotionDilate;

    const bool shouldCommitGuideHistory =
        (RtPostModeCommitsHistory(rtPostMode) && advancedSplitHistory) ||
        restirCanCommitGuideHistory;

    if (shouldCommitGuideHistory)
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

        const bool shouldCommitViewZHistory =
            ranViewZReconstruct &&
            ranMotionDilate &&
            (temporalWouldRun || restirNeedsTemporalGuides);

        if (shouldCommitViewZHistory)
        {
            CommitRtViewZHistory(cl, writeIndex);
        }
        else if (!temporalWouldRun && !restirNeedsTemporalGuides)
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

void Renderer::WriteNullRtEnvAliasSrv(D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Device* device = m_device5.Get();
    if (!device)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = 1;
    srv.Buffer.StructureByteStride = sizeof(RtEnvAliasEntry);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(nullptr, &srv, dst);
}

void Renderer::WriteRtEnvAliasSrv(D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    if (!m_device5 ||
        !m_rtEnvAliasReady ||
        !m_rtEnvAliasBuffer ||
        m_rtEnvAliasCount == 0)
    {
        WriteNullRtEnvAliasSrv(dst);
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = m_rtEnvAliasCount;
    srv.Buffer.StructureByteStride = sizeof(RtEnvAliasEntry);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    m_device5->CreateShaderResourceView(
        m_rtEnvAliasBuffer.Get(),
        &srv,
        dst);
}

bool Renderer::EnsureRtEnvironmentAlias(ID3D12Device* device, CommandList& cl)
{
    if (!device)
        return false;

    if (!m_rtEnvAliasDirty &&
        m_rtEnvAliasReady &&
        m_rtEnvAliasBuffer &&
        m_rtEnvAliasCount > 0)
    {
        return true;
    }

    bool built = false;

    if (m_rtEnvCpuRadianceReady &&
        !m_rtEnvCpuRadiance.empty() &&
        m_rtEnvCpuFaceSize > 0)
    {
        built = m_rtEnvImportance.BuildFromCubeFaces(
            m_rtEnvCpuRadiance,
            m_rtEnvCpuFaceSize);
    }

    if (!built)
    {
        const uint32_t fallbackFaceSize =
            (m_rtEnvFaceSize > 0)
            ? m_rtEnvFaceSize
            : kRtEnvImportanceFallbackFaceSize;

        built = m_rtEnvImportance.BuildUniformFallback(fallbackFaceSize);
    }

    std::string validateError;
    if (!built || !m_rtEnvImportance.Validate(&validateError))
    {
        built = m_rtEnvImportance.BuildUniformFallback(
            kRtEnvImportanceFallbackFaceSize);
    }

    validateError.clear();
    if (!built || !m_rtEnvImportance.Validate(&validateError))
    {
        m_rtEnvAliasReady = false;
        m_rtEnvAliasDirty = true;
        m_rtEnvAliasCount = 0;
        m_rtEnvFaceSize = 0;
        m_rtEnvTotalWeight = 0.0f;
        m_rtEnvAliasFallback = true;
        return false;
    }

    const std::vector<RtEnvAliasEntry>& entries = m_rtEnvImportance.Entries();
    if (entries.empty())
    {
        m_rtEnvAliasReady = false;
        m_rtEnvAliasDirty = true;
        m_rtEnvAliasCount = 0;
        m_rtEnvFaceSize = 0;
        m_rtEnvTotalWeight = 0.0f;
        m_rtEnvAliasFallback = true;
        return false;
    }

    const uint64_t byteSize =
        static_cast<uint64_t>(entries.size()) *
        static_cast<uint64_t>(sizeof(RtEnvAliasEntry));

    ComPtr<ID3D12Resource> aliasBuffer;
    ComPtr<ID3D12Resource> aliasUpload;

    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

        ThrowIfFailed(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&aliasBuffer)),
            "Create RT environment alias buffer");

        SetD3D12ObjectName(aliasBuffer.Get(), L"RT Environment Alias Buffer");
        CommandList::SetGlobalState(aliasBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    }

    {
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

        ThrowIfFailed(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&aliasUpload)),
            "Create RT environment alias upload buffer");

        SetD3D12ObjectName(aliasUpload.Get(), L"RT Environment Alias Upload");
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };

    ThrowIfFailed(
        aliasUpload->Map(0, &readRange, &mapped),
        "Map RT environment alias upload buffer");

    std::memcpy(
        mapped,
        entries.data(),
        static_cast<size_t>(byteSize));

    D3D12_RANGE writtenRange{ 0, static_cast<SIZE_T>(byteSize) };
    aliasUpload->Unmap(0, &writtenRange);

    cl.Get()->CopyBufferRegion(
        aliasBuffer.Get(),
        0,
        aliasUpload.Get(),
        0,
        byteSize);

    cl.Transition(
        aliasBuffer.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.FlushBarriers();

    m_rtEnvAliasBuffer = aliasBuffer;
    m_rtEnvAliasUpload = aliasUpload;

    m_rtEnvAliasCount = m_rtEnvImportance.AliasCount();
    m_rtEnvFaceSize = m_rtEnvImportance.FaceSize();
    m_rtEnvTotalWeight = m_rtEnvImportance.TotalWeight();
    m_rtEnvAliasFallback = m_rtEnvImportance.IsFallback();

    m_rtEnvAliasReady = true;
    m_rtEnvAliasDirty = false;

    return true;
}

bool Renderer::LoadRtEnvironmentCpuRadiance(const std::filesystem::path& path)
{
    ClearRtEnvironmentCpuRadiance();

    if (path.empty() || !std::filesystem::exists(path))
    {
        DebugOutput("Warning: RT environment CPU radiance source missing. Using uniform alias fallback.");
        return false;
    }

    std::vector<DirectX::XMFLOAT3> radiance;

    if (!LoadLatLongEnvironmentAsCubeFaces(
        path,
        kRtEnvImportanceFaceSize,
        radiance))
    {
        DebugOutput("Warning: failed to load RT environment CPU radiance. Using uniform alias fallback.");
        return false;
    }

    const size_t expectedCount =
        static_cast<size_t>(6u) *
        static_cast<size_t>(kRtEnvImportanceFaceSize) *
        static_cast<size_t>(kRtEnvImportanceFaceSize);

    if (radiance.size() != expectedCount)
    {
        DebugOutput("Warning: RT environment CPU radiance size mismatch. Using uniform alias fallback.");
        return false;
    }

    m_rtEnvCpuRadiance = std::move(radiance);
    m_rtEnvCpuFaceSize = kRtEnvImportanceFaceSize;
    m_rtEnvCpuRadianceReady = true;

    m_rtEnvAliasDirty = true;
    m_rtEnvAliasReady = false;
    m_rtEnvAliasBuffer.Reset();
    m_rtEnvAliasUpload.Reset();

    m_rtEnvAliasCount = 0;
    m_rtEnvFaceSize = 0;
    m_rtEnvTotalWeight = 0.0f;
    m_rtEnvAliasFallback = true;
    ++m_rtEnvAliasVersion;

    DebugOutput("RT environment CPU radiance loaded for importance sampling.");
    return true;
}

void Renderer::ClearRtEnvironmentCpuRadiance()
{
    const bool hadEnvSource =
        m_rtEnvCpuRadianceReady ||
        m_rtEnvAliasReady ||
        !m_rtEnvCpuRadiance.empty();

    m_rtEnvCpuRadiance.clear();
    m_rtEnvCpuFaceSize = 0;
    m_rtEnvCpuRadianceReady = false;

    m_rtEnvAliasDirty = true;
    m_rtEnvAliasReady = false;
    m_rtEnvAliasBuffer.Reset();
    m_rtEnvAliasUpload.Reset();

    m_rtEnvAliasCount = 0;
    m_rtEnvFaceSize = 0;
    m_rtEnvTotalWeight = 0.0f;
    m_rtEnvAliasFallback = true;

    if (hadEnvSource)
        ++m_rtEnvAliasVersion;
}

void Renderer::ResetRtRestirResources()
{
    m_rtRestirInitialReservoir.Reset();

    for (auto& r : m_rtRestirTemporalReservoir)
        r.Reset();

    m_rtRestirSpatialReservoir.Reset();

    m_rtRestirResolvedDiffuse.Reset();
    m_rtRestirResolvedSpec.Reset();

    m_rtRestirAppliedDiffuse.Reset();
    m_rtRestirAppliedSpec.Reset();
    m_rtRestirAppliedReady = false;

    m_rtRestirResourcesReady = false;
    ResetRtRestirHistory();
}

void Renderer::EnsureRtRestirResources(uint32_t width, uint32_t height)
{
    if (!m_device5 || width == 0 || height == 0)
        return;

    ID3D12Device* device = m_device5.Get();

    if (!m_rtOutputUav.IsValid())
        m_rtOutputUav = m_srvHeap.Allocate(kRtUavTableCount);

    auto WriteNullRestirUavs = [&]()
    {
        if (!m_rtOutputUav.IsValid())
            return;

        // u9 null structured-buffer UAV
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = 1;
            uav.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
            uav.Buffer.CounterOffsetInBytes = 0;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

            device->CreateUnorderedAccessView(
                nullptr,
                nullptr,
                &uav,
                RtUavCpuAt(9));
        }

        // u10/u11 null RGBA16 UAVs
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

            device->CreateUnorderedAccessView(
                nullptr,
                nullptr,
                &uav,
                RtUavCpuAt(10));

            device->CreateUnorderedAccessView(
                nullptr,
                nullptr,
                &uav,
                RtUavCpuAt(11));
        }
    };

    auto WriteLiveRestirUavs = [&]()
    {
        if (!m_rtOutputUav.IsValid() ||
            !m_rtRestirInitialReservoir ||
            !m_rtRestirResolvedDiffuse ||
            !m_rtRestirResolvedSpec)
        {
            WriteNullRestirUavs();
            return false;
        }

        const uint64_t pixelCount64 =
            static_cast<uint64_t>(width) *
            static_cast<uint64_t>(height);

        if (pixelCount64 == 0 || pixelCount64 > UINT32_MAX)
        {
            WriteNullRestirUavs();
            return false;
        }

        const UINT pixelCount = static_cast<UINT>(pixelCount64);

        // u9 = initial reservoir
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.FirstElement = 0;
            uav.Buffer.NumElements = pixelCount;
            uav.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
            uav.Buffer.CounterOffsetInBytes = 0;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

            device->CreateUnorderedAccessView(
                m_rtRestirInitialReservoir.Get(),
                nullptr,
                &uav,
                RtUavCpuAt(9));
        }

        // u10/u11 = resolved diffuse/spec
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

            device->CreateUnorderedAccessView(
                m_rtRestirResolvedDiffuse.Get(),
                nullptr,
                &uav,
                RtUavCpuAt(10));

            device->CreateUnorderedAccessView(
                m_rtRestirResolvedSpec.Get(),
                nullptr,
                &uav,
                RtUavCpuAt(11));
        }

        return true;
    };

    const bool wantsRestir =
        m_rtEnableRestirEnvDi ||
        IsRtRestirDebug(m_debugView);

    if (!wantsRestir)
    {
        // Do not destroy ReSTIR resources just because the current frame does not
        // need ReSTIR. They may still be referenced by recently submitted GPU work,
        // and keeping them alive also avoids churn while cycling debug views.
        //
        // The shared DXR UAV table is still made deterministic for non-ReSTIR
        // frames by binding null descriptors to the ReSTIR UAV slots.

        m_rtRestirTemporalValidThisFrame = false;
        m_rtRestirSpatialValidThisFrame = false;
        m_rtRestirResolvedValidThisFrame = false;
        m_rtRestirAppliedReady = false;

        // Keep the expanded DXR UAV table deterministic even when ReSTIR is off.
        WriteNullRestirUavs();
        return;
    }

    const bool needCreate =
        !m_rtRestirResourcesReady ||
        !m_rtRestirInitialReservoir ||
        !m_rtRestirTemporalReservoir[0] ||
        !m_rtRestirTemporalReservoir[1] ||
        !m_rtRestirSpatialReservoir ||
        !m_rtRestirResolvedDiffuse ||
        !m_rtRestirResolvedSpec ||
        !m_rtRestirAppliedDiffuse ||
        !m_rtRestirAppliedSpec ||
        m_rtOutputWidth != width ||
        m_rtOutputHeight != height;

    if (!needCreate)
    {
        // Resources may have stayed alive while the previous non-ReSTIR frame
        // wrote null UAVs into u9/u10/u11. Rebind the live descriptors every time
        // ReSTIR is wanted and resources already exist.
        if (!WriteLiveRestirUavs())
        {
            m_rtRestirResourcesReady = false;
            m_rtRestirAppliedReady = false;
            ResetRtRestirHistory();
            return;
        }

        m_rtRestirResourcesReady = true;
        m_rtRestirAppliedReady =
            m_rtRestirAppliedDiffuse &&
            m_rtRestirAppliedSpec;

        return;
    }
    ResetRtRestirResources();

    const uint64_t pixelCount64 =
        static_cast<uint64_t>(width) *
        static_cast<uint64_t>(height);

    if (pixelCount64 == 0 || pixelCount64 > UINT32_MAX)
    {
        WriteNullRestirUavs();
        return;
    }

    const UINT pixelCount = static_cast<UINT>(pixelCount64);

    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

    auto CreateReservoirBuffer =
        [&](ComPtr<ID3D12Resource>& resource, const wchar_t* name)
    {
        const uint64_t byteSize =
            pixelCount64 * static_cast<uint64_t>(sizeof(RtRestirReservoir));

        auto desc = CD3DX12_RESOURCE_DESC::Buffer(
            byteSize,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        ThrowIfFailed(
            device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(&resource)),
            "Create RT ReSTIR reservoir buffer");

        SetD3D12ObjectName(resource.Get(), name);

        CommandList::SetGlobalState(
            resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };

    auto CreateRgba16UavTexture =
        [&](ComPtr<ID3D12Resource>& resource, const wchar_t* name)
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
            "Create RT ReSTIR resolved texture");

        SetD3D12ObjectName(resource.Get(), name);

        CommandList::SetGlobalState(
            resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };

    CreateReservoirBuffer(
        m_rtRestirInitialReservoir,
        L"RT ReSTIR Initial Reservoir");

    CreateReservoirBuffer(
        m_rtRestirTemporalReservoir[0],
        L"RT ReSTIR Temporal Reservoir 0");

    CreateReservoirBuffer(
        m_rtRestirTemporalReservoir[1],
        L"RT ReSTIR Temporal Reservoir 1");

    CreateReservoirBuffer(
        m_rtRestirSpatialReservoir,
        L"RT ReSTIR Spatial Reservoir");

    CreateRgba16UavTexture(
        m_rtRestirResolvedDiffuse,
        L"RT ReSTIR Resolved Diffuse");

    CreateRgba16UavTexture(
        m_rtRestirResolvedSpec,
        L"RT ReSTIR Resolved Spec");

    CreateRgba16UavTexture(
        m_rtRestirAppliedDiffuse,
        L"RT ReSTIR Applied Diffuse");

    CreateRgba16UavTexture(
        m_rtRestirAppliedSpec,
        L"RT ReSTIR Applied Spec");

    // DXR global UAV table slots:
    // u9  = initial reservoir
    // u10 = resolved diffuse
    // u11 = resolved spec
    if (!WriteLiveRestirUavs())
    {
        m_rtRestirResourcesReady = false;
        ResetRtRestirHistory();
        return;
    }

    m_rtRestirResourcesReady = true;
    m_rtRestirAppliedReady = true;
    ResetRtRestirHistory();
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtRestirTemporalConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height,
    bool temporalHistoryValid,
    bool surfaceIdHistoryValid,
    bool viewZHistoryValid)
{
    constexpr uint32_t cbSize =
        (sizeof(RtRestirTemporalConstants) + 255u) & ~255u;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<RtRestirTemporalConstants*>(alloc.cpu);

    *cb = {};

    cb->invResolution =
    {
        width > 0 ? 1.0f / static_cast<float>(width) : 0.0f,
        height > 0 ? 1.0f / static_cast<float>(height) : 0.0f
    };

    cb->temporalEnabled =
        m_rtRestirUseTemporal ? 1u : 0u;

    cb->historyValid =
        temporalHistoryValid ? 1u : 0u;

    cb->surfaceIdHistoryValid =
        surfaceIdHistoryValid ? 1u : 0u;

    cb->viewZHistoryValid =
        viewZHistoryValid ? 1u : 0u;

    cb->debugView = m_debugView;
    cb->frameIndex = frameIndex;

    cb->depthSigma = std::max(1e-5f, m_rtRestirDepthSigma);
    cb->normalSigma = std::max(1e-5f, m_rtRestirNormalSigma);
    cb->roughnessSigma = std::max(1e-5f, m_rtRestirRoughnessSigma);
    cb->viewZSigma = std::max(1e-5f, m_rtRestirViewZSigma);

    cb->reprojectMinWeight =
        std::clamp(m_rtRestirTemporalMinConfidence, 0.0f, 1.0f);

    cb->maxM =
        std::max(1.0f, m_rtRestirMaxM);

    cb->maxAge =
        std::max(0.0f, m_rtRestirMaxAge);

    cb->maxWeight =
        std::max(1.0f, m_rtRestirMaxWeight);

    return alloc.gpu;
}

bool Renderer::UpdateRtRestirTemporalTables(
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
    uint32_t height)
{
    if (!device ||
        !m_rtRestirInitialReservoir ||
        !outTemporalReservoir ||
        !currPrevUvResource ||
        !m_rtAovNormal ||
        !m_rtAovDepth ||
        !m_rtAovSurfaceId ||
        !currViewZResource ||
        !prevViewZResource ||
        !prevNormalResource ||
        !prevDepthResource ||
        !prevSurfaceIdResource ||
        !m_rtOutput ||
        !m_rtRestirResourcesReady ||
        !m_rtAovReady ||
        !m_rtAovSurfaceIdReady ||
        !m_rtOutputReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.restirTemporalSrvTable,
        frame.restirTemporalSrvCount,
        kRtRestirTemporalSrvCount);

    EnsureRtDescriptorTable(
        frame.restirTemporalUavTable,
        frame.restirTemporalUavCount,
        kRtRestirTemporalUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirTemporalSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirTemporalUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    const uint32_t pixelCount =
        std::max(1u, width * height);

    auto WriteReservoirSrv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = pixelCount;
        srv.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    auto WriteReservoirUav = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = pixelCount;
        uav.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            UavAt(slot));
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

    // t0 = current initial reservoir from DXR section 8.
    WriteReservoirSrv(m_rtRestirInitialReservoir.Get(), 0);

    // t1 = previous temporal reservoir. Null is valid when historyValid == 0.
    WriteReservoirSrv(prevTemporalReservoir, 1);

    // t2 = current normal/roughness.
    WriteSrv2D(m_rtAovNormal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2);

    // t3 = current depth.
    WriteSrv2D(m_rtAovDepth.Get(), DXGI_FORMAT_R32_FLOAT, 3);

    // t4 = current previous-UV guide.
    // Important: this should be the dilated previous UV from MotionDilate,
    // not the raw DXR motion AOV.
    WriteSrv2D(currPrevUvResource, DXGI_FORMAT_R16G16_FLOAT, 4);

    // t5 = current reconstructed ViewZ, or raw ViewZ fallback.
    WriteSrv2D(currViewZResource, DXGI_FORMAT_R16_FLOAT, 5);

    // t6 = current SurfaceId.
    WriteSrv2D(m_rtAovSurfaceId.Get(), DXGI_FORMAT_R32_UINT, 6);

    // t7 = previous normal/roughness history.
    WriteSrv2D(prevNormalResource, DXGI_FORMAT_R16G16B16A16_FLOAT, 7);

    // t8 = previous depth history.
    WriteSrv2D(prevDepthResource, DXGI_FORMAT_R32_FLOAT, 8);

    // t9 = previous reconstructed ViewZ history, or fallback.
    WriteSrv2D(prevViewZResource, DXGI_FORMAT_R16_FLOAT, 9);

    // t10 = previous SurfaceId history.
    WriteSrv2D(prevSurfaceIdResource, DXGI_FORMAT_R32_UINT, 10);

    // u0 = current temporal reservoir output.
    WriteReservoirUav(outTemporalReservoir, 0);

    // u1 = debug output.
    // Bound consistently because the shader declares it. RunRtRestirTemporal()
    // transitions m_rtOutput to UAV before dispatch.
    WriteUav2D(m_rtOutput.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);

    return true;
}

bool Renderer::RunRtRestirTemporal(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    const RtDenoiserGuides& guides,
    bool ranViewZReconstruct,
    bool ranMotionDilate,
    const RtDebugRouting& rtDebug)
{
    if (!device ||
        !m_rtRestirResourcesReady ||
        !m_rtRestirInitialReservoir ||
        !m_rtRestirTemporalReservoir[0] ||
        !m_rtRestirTemporalReservoir[1] ||
        !guides.normalRough ||
        !guides.depth ||
        !guides.surfaceId ||
        !m_rtOutput ||
        !m_rtOutputReady)
    {
        return false;
    }

    const bool wantsRestirTemporal =
        m_rtEnableRestirEnvDi ||
        rtDebug.wantsRtRestirTemporalDebug ||
        rtDebug.wantsRtRestirSpatialDebug ||
        rtDebug.wantsRtRestirResolveDebug;

    if (!wantsRestirTemporal)
        return false;

    ID3D12Resource* currPrevUv =
        (ranMotionDilate && guides.prevUVDilated)
        ? guides.prevUVDilated
        : guides.prevUVRaw;

    if (!currPrevUv)
    {
        // The shader declares a prev-UV SRV, so bindable input is required even
        // when history reuse is disabled. Usually raw DXR motion is the fallback.
        m_rtRestirHistoryValid = false;
        ResetRtRestirHistory();
        return false;
    }

    const uint32_t readIndex = m_rtRestirHistoryReadIndex;
    const uint32_t writeIndex = m_rtRestirHistoryWriteIndex;

    ID3D12Resource* outTemporalReservoir =
        m_rtRestirTemporalReservoir[writeIndex].Get();

    RtDenoiserHistories histories =
        BuildRtDenoiserHistories(
            m_rtHistoryReadIndex,
            1u - m_rtHistoryReadIndex);

    ID3D12Resource* currViewZ =
        (ranViewZReconstruct && guides.viewZRecons)
        ? guides.viewZRecons
        : m_rtAovViewZRaw.Get();

    // Descriptor fallback is allowed, but reuse is not.
    // The shader tables always bind valid SRVs, but history flags must only be
    // true when those SRVs are real previous-frame histories.
    const bool havePreviousGuideHistory =
        m_rtTemporalHistoryValid &&
        histories.normalRead &&
        histories.depthRead &&
        m_rtSurfaceIdHistoryValid &&
        histories.surfaceIdRead &&
        ranViewZReconstruct &&
        m_rtViewZHistoryValid &&
        guides.viewZHistoryRead &&
        currViewZ;

    const bool canUseTemporalHistory =
        m_rtRestirUseTemporal &&
        m_rtRestirHistoryValid &&
        ranMotionDilate &&
        guides.prevUVDilated &&
        havePreviousGuideHistory;

    ID3D12Resource* prevTemporalReservoir =
        canUseTemporalHistory
        ? m_rtRestirTemporalReservoir[readIndex].Get()
        : nullptr;

    ID3D12Resource* prevNormal =
        canUseTemporalHistory
        ? histories.normalRead
        : guides.normalRough;

    ID3D12Resource* prevDepth =
        canUseTemporalHistory
        ? histories.depthRead
        : guides.depth;

    ID3D12Resource* prevViewZ =
        canUseTemporalHistory
        ? guides.viewZHistoryRead
        : currViewZ;

    ID3D12Resource* prevSurfaceId =
        canUseTemporalHistory
        ? histories.surfaceIdRead
        : guides.surfaceId;

    const bool surfaceIdHistoryValid = canUseTemporalHistory;
    const bool viewZHistoryValid = canUseTemporalHistory;

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT ReSTIR Temporal");

    cl.Transition(m_rtRestirInitialReservoir.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (prevTemporalReservoir)
    {
        cl.Transition(prevTemporalReservoir, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    cl.Transition(outTemporalReservoir, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(currPrevUv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(guides.surfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (currViewZ)
        cl.Transition(currViewZ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (prevNormal && prevNormal != guides.normalRough)
    {
        cl.Transition(prevNormal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (prevDepth && prevDepth != guides.depth)
    {
        cl.Transition(prevDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (prevViewZ && prevViewZ != currViewZ)
    {
        cl.Transition(prevViewZ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (prevSurfaceId && prevSurfaceId != guides.surfaceId)
    {
        cl.Transition(prevSurfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Bound as u1 for debug 106/107. Keeping the transition here is simple and
    // matches your other debug-capable compute passes.
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl.FlushBarriers();

    const bool okTables =
        UpdateRtRestirTemporalTables(
            frameIndex,
            device,
            prevTemporalReservoir,
            outTemporalReservoir,
            currPrevUv,
            currViewZ,
            prevViewZ,
            prevNormal,
            prevDepth,
            prevSurfaceId,
            width,
            height);

    if (!okTables)
    {
        m_rtRestirTemporalValidThisFrame = false;
        m_rtRestirHistoryValid = false;
        ResetRtRestirHistory();
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtRestirTemporalConstants(
            frameIndex,
            width,
            height,
            canUseTemporalHistory,
            surfaceIdHistoryValid,
            viewZHistoryValid);

    m_rtRestirTemporalPass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].restirTemporalSrvTable.gpu,
        m_rtFrames[frameIndex].restirTemporalUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[2]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = outTemporalReservoir;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(2, barriers);

    // Swap so the current frame's temporal output is readable by spatial/resolve.
    m_rtRestirHistoryReadIndex = writeIndex;
    m_rtRestirHistoryWriteIndex = readIndex;

    m_rtRestirTemporalValidThisFrame = true;

    m_rtRestirHistoryValid =
        m_rtRestirUseTemporal &&
        m_rtRestirTemporalValidThisFrame &&
        m_rtRestirTemporalReservoir[m_rtRestirHistoryReadIndex].Get() != nullptr;

    CmdEndEvent(cmdList);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtRestirSpatialConstants(
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height)
{
    constexpr uint32_t cbSize =
        (sizeof(RtRestirSpatialConstants) + 255u) & ~255u;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<RtRestirSpatialConstants*>(alloc.cpu);

    *cb = {};

    cb->invResolution =
    {
        width > 0 ? 1.0f / static_cast<float>(width) : 0.0f,
        height > 0 ? 1.0f / static_cast<float>(height) : 0.0f
    };

    cb->sampleCount =
        std::max(1u, m_rtRestirSpatialSamples);

    cb->radius =
        std::max(1u, m_rtRestirSpatialRadius);

    cb->normalSigma =
        std::max(1e-5f, m_rtRestirNormalSigma);

    cb->depthSigma =
        std::max(1e-5f, m_rtRestirDepthSigma);

    cb->roughnessSigma =
        std::max(1e-5f, m_rtRestirRoughnessSigma);

    cb->viewZSigma =
        std::max(1e-5f, m_rtRestirViewZSigma);

    cb->maxM =
        std::max(1.0f, m_rtRestirMaxM);

    cb->maxWeight =
        std::max(1.0f, m_rtRestirMaxWeight);

    cb->frameIndex = frameIndex;
    cb->debugView = m_debugView;

    cb->distanceNormParams =
    {
        1.0f,
        0.0f,
        1.0f
    };

    cb->distanceNormSigma = 0.08f;

    return alloc.gpu;
}

bool Renderer::UpdateRtRestirSpatialTables(
    uint32_t frameIndex,
    ID3D12Device* device,
    ID3D12Resource* temporalReservoir,
    ID3D12Resource* currNormalResource,
    ID3D12Resource* currDepthResource,
    ID3D12Resource* currSurfaceIdResource,
    ID3D12Resource* currViewZResource,
    ID3D12Resource* outSpatialReservoir,
    uint32_t width,
    uint32_t height)
{
    if (!device ||
        !temporalReservoir ||
        !currNormalResource ||
        !currDepthResource ||
        !currSurfaceIdResource ||
        !currViewZResource ||
        !outSpatialReservoir ||
        !m_rtOutput ||
        !m_rtOutputReady ||
        !m_rtRestirResourcesReady)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.restirSpatialSrvTable,
        frame.restirSpatialSrvCount,
        kRtRestirSpatialSrvCount);

    EnsureRtDescriptorTable(
        frame.restirSpatialUavTable,
        frame.restirSpatialUavCount,
        kRtRestirSpatialUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirSpatialSrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirSpatialUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    const uint32_t pixelCount =
        std::max(1u, width * height);

    auto WriteReservoirSrv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = pixelCount;
        srv.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    auto WriteReservoirUav = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = pixelCount;
        uav.Buffer.StructureByteStride = sizeof(RtRestirReservoir);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            UavAt(slot));
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

    // t0 = temporal reservoir
    WriteReservoirSrv(temporalReservoir, 0);

    // t1 = current normal/roughness
    WriteSrv2D(currNormalResource, DXGI_FORMAT_R16G16B16A16_FLOAT, 1);

    // t2 = current depth
    WriteSrv2D(currDepthResource, DXGI_FORMAT_R32_FLOAT, 2);

    // t3 = current SurfaceId
    WriteSrv2D(currSurfaceIdResource, DXGI_FORMAT_R32_UINT, 3);

    // t4 = current ViewZ
    WriteSrv2D(currViewZResource, DXGI_FORMAT_R16_FLOAT, 4);

    // u0 = spatial reservoir
    WriteReservoirUav(outSpatialReservoir, 0);

    // u1 = debug output
    WriteUav2D(m_rtOutput.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);

    return true;
}

bool Renderer::RunRtRestirSpatial(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    const RtDenoiserGuides& guides,
    bool ranViewZReconstruct,
    bool ranRestirTemporal,
    const RtDebugRouting& rtDebug)
{
    if (!device ||
        !m_rtRestirResourcesReady ||
        !m_rtRestirSpatialReservoir ||
        !m_rtOutput ||
        !m_rtOutputReady ||
        !guides.normalRough ||
        !guides.depth ||
        !guides.surfaceId)
    {
        return false;
    }

    const bool wantsRestirSpatial =
        (m_rtEnableRestirEnvDi && m_rtRestirUseSpatial) ||
        rtDebug.wantsRtRestirSpatialDebug ||
        rtDebug.wantsRtRestirResolveDebug;

    if (!wantsRestirSpatial)
        return false;

    if (!ranRestirTemporal || !m_rtRestirTemporalValidThisFrame)
        return false;

    ID3D12Resource* temporalReservoir =
        m_rtRestirTemporalReservoir[m_rtRestirHistoryReadIndex].Get();

    if (!temporalReservoir)
        return false;

    ID3D12Resource* currViewZ =
        (ranViewZReconstruct && guides.viewZRecons)
        ? guides.viewZRecons
        : guides.viewZRaw;

    if (!currViewZ)
        return false;

    ID3D12Resource* outSpatialReservoir =
        m_rtRestirSpatialReservoir.Get();

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT ReSTIR Spatial");

    cl.Transition(temporalReservoir, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(guides.normalRough, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(guides.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(guides.surfaceId, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(currViewZ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.Transition(outSpatialReservoir, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Bound as u1 for debug 108/109.
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl.FlushBarriers();

    const bool okTables =
        UpdateRtRestirSpatialTables(
            frameIndex,
            device,
            temporalReservoir,
            guides.normalRough,
            guides.depth,
            guides.surfaceId,
            currViewZ,
            outSpatialReservoir,
            width,
            height);

    if (!okTables)
    {
        m_rtRestirSpatialValidThisFrame = false;
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        UpdateRtRestirSpatialConstants(
            frameIndex,
            width,
            height);

    m_rtRestirSpatialPass.Dispatch(
        cl,
        cb,
        m_rtFrames[frameIndex].restirSpatialSrvTable.gpu,
        m_rtFrames[frameIndex].restirSpatialUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[2]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].UAV.pResource = outSpatialReservoir;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(2, barriers);

    m_rtRestirSpatialValidThisFrame = true;

    CmdEndEvent(cmdList);
    return true;
}

bool Renderer::RunRtRestirResolve(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    float sceneTime,
    const RtDebugRouting& rtDebug)
{
    if (!device ||
        !m_device5 ||
        !m_rtPipeline.StateObject() ||
        !m_rtRestirResourcesReady ||
        !m_rtRestirResolvedDiffuse ||
        !m_rtRestirResolvedSpec ||
        !m_rtOutputReady ||
        m_rtFrames[frameIndex].tlas.GpuAddress() == 0)
    {
        return false;
    }

    const bool wantsResolve =
        (m_rtEnableRestirEnvDi && m_rtRestirResolveToBeauty) ||
        rtDebug.wantsRtRestirResolveDebug;

    if (!wantsResolve)
        return false;

    // Resolve consumes the best reservoir produced this frame:
    //   spatial  -> preferred when enabled and valid
    //   temporal -> fallback when enabled and valid
    //   initial  -> last-resort debug/validation fallback
    //
    // Resolve is a DXR visibility/material pass. It consumes the selected
    // reservoir through t34 and must not regenerate initial reservoirs.
    ID3D12Resource* resolveReservoir = nullptr;

    if (m_rtRestirUseSpatial &&
        m_rtRestirSpatialValidThisFrame &&
        m_rtRestirSpatialReservoir)
    {
        resolveReservoir = m_rtRestirSpatialReservoir.Get();
    }
    else if (m_rtRestirUseTemporal && m_rtRestirTemporalValidThisFrame &&
        m_rtRestirTemporalReservoir[m_rtRestirHistoryReadIndex])
    {
        resolveReservoir =
            m_rtRestirTemporalReservoir[m_rtRestirHistoryReadIndex].Get();
    }
    else if (m_rtRestirInitialReservoir)
    {
        // Fallback useful for bring-up/debug if temporal/spatial did not run.
        resolveReservoir = m_rtRestirInitialReservoir.Get();
    }

    if (!resolveReservoir)
        return false;

    auto cmd4 = GetCommandList4(cl);
    if (!cmd4)
        return false;

    auto* cmdList = cl.Get();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.GetHeap() };
    cmd4->SetDescriptorHeaps(1, heaps);

    CmdBeginEvent(cmdList, "RT ReSTIR Resolve");

    cl.Transition(resolveReservoir, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtRestirResolvedDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtRestirResolvedSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Bound as g_Output for debug 110..114.
    cl.Transition(m_rtOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl.FlushBarriers();

    // Rebind t34 to the reservoir selected for resolve.
    UpdateRtGeometryTable(
        frameIndex,
        resolveReservoir);

    cmd4->SetPipelineState1(m_rtPipeline.StateObject());
    cmd4->SetComputeRootSignature(m_rtPipeline.GlobalRootSignature());

    cmd4->SetComputeRootDescriptorTable(0, m_rtOutputUav.gpu);
    cmd4->SetComputeRootShaderResourceView(
        1,
        m_rtFrames[frameIndex].tlas.GpuAddress());

    const D3D12_GPU_VIRTUAL_ADDRESS perFrameCb =
        UpdateGlobalConstants(
            frameIndex,
            width,
            height,
            sceneTime);

    const D3D12_GPU_VIRTUAL_ADDRESS rtRayGenCb =
        UpdateRtRayGenConstants(
            frameIndex,
            1u); // ReSTIR resolve mode

    cmd4->SetComputeRootConstantBufferView(2, perFrameCb);
    cmd4->SetComputeRootDescriptorTable(3, m_rtFrames[frameIndex].geometryTable.gpu);
    cmd4->SetComputeRootConstantBufferView(4, rtRayGenCb);

    auto tableBase =
        m_rtPipeline.ShaderTable()->GetGPUVirtualAddress();

    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.RayGenerationShaderRecord.StartAddress =
        tableBase + m_rtPipeline.RayGenOffset();
    rays.RayGenerationShaderRecord.SizeInBytes =
        m_rtPipeline.RayGenRecordSize();

    rays.MissShaderTable.StartAddress =
        tableBase + m_rtPipeline.MissOffset();
    rays.MissShaderTable.SizeInBytes =
        m_rtPipeline.MissRecordSize();
    rays.MissShaderTable.StrideInBytes =
        m_rtPipeline.MissRecordSize();

    rays.HitGroupTable.StartAddress =
        tableBase + m_rtPipeline.HitGroupOffset();
    rays.HitGroupTable.SizeInBytes =
        m_rtPipeline.HitGroupRecordSize();
    rays.HitGroupTable.StrideInBytes =
        m_rtPipeline.HitGroupRecordSize();

    rays.Width = width;
    rays.Height = height;
    rays.Depth = 1;

    cmd4->DispatchRays(&rays);

    D3D12_RESOURCE_BARRIER barriers[3]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].UAV.pResource = m_rtRestirResolvedDiffuse.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].UAV.pResource = m_rtRestirResolvedSpec.Get();

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[2].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[2].UAV.pResource = m_rtOutput.Get();

    cmdList->ResourceBarrier(3, barriers);

    m_rtRestirResolvedValidThisFrame = true;

    CmdEndEvent(cmdList);
    return true;
}

// ReSTIR reservoir history depends on both reservoir settings and the guide
// histories used to validate reuse. Raw accumulation resets alone should not
// clear ReSTIR history, but true temporal/guide invalidation must.
void Renderer::ResetRtRestirHistory()
{
    m_rtRestirHistoryValid = false;

    m_rtRestirTemporalValidThisFrame = false;
    m_rtRestirSpatialValidThisFrame = false;
    m_rtRestirResolvedValidThisFrame = false;

    m_rtRestirHistoryReadIndex = 0;
    m_rtRestirHistoryWriteIndex = 1;
}

bool Renderer::UpdateRtRestirApplyTables(
    uint32_t frameIndex,
    ID3D12Device* device,
    ID3D12Resource* baseDiffuse,
    ID3D12Resource* baseSpec,
    ID3D12Resource* restirDiffuse,
    ID3D12Resource* restirSpec,
    ID3D12Resource* outDiffuse,
    ID3D12Resource* outSpec)
{
    if (!device ||
        !baseDiffuse ||
        !baseSpec ||
        !restirDiffuse ||
        !restirSpec ||
        !outDiffuse ||
        !outSpec)
    {
        return false;
    }

    auto& frame = m_rtFrames[frameIndex];

    EnsureRtDescriptorTable(
        frame.restirApplySrvTable,
        frame.restirApplySrvCount,
        kRtRestirApplySrvCount);

    EnsureRtDescriptorTable(
        frame.restirApplyUavTable,
        frame.restirApplyUavCount,
        kRtRestirApplyUavCount);

    const uint32_t descriptorSize = m_srvHeap.DescriptorSize();

    auto SrvAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirApplySrvTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto UavAt = [&](uint32_t slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = frame.restirApplyUavTable.cpu;
        h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        return h;
    };

    auto WriteTextureSrv = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;

        device->CreateShaderResourceView(
            resource,
            &srv,
            SrvAt(slot));
    };

    auto WriteTextureUav = [&](ID3D12Resource* resource, uint32_t slot)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = 0;
        uav.Texture2D.PlaneSlice = 0;

        device->CreateUnorderedAccessView(
            resource,
            nullptr,
            &uav,
            UavAt(slot));
    };

    // t0 = base diffuse
    // t1 = base spec
    // t2 = resolved ReSTIR diffuse
    // t3 = resolved ReSTIR spec
    WriteTextureSrv(baseDiffuse, 0);
    WriteTextureSrv(baseSpec, 1);
    WriteTextureSrv(restirDiffuse, 2);
    WriteTextureSrv(restirSpec, 3);

    // u0 = applied diffuse
    // u1 = applied spec
    WriteTextureUav(outDiffuse, 0);
    WriteTextureUav(outSpec, 1);

    return true;
}

// Optional validation apply for resolved ReSTIR environment lighting.
// The resolved diffuse/specular terms are added after the existing RT signal
// pipeline, so this is useful for inspection but is not equivalent to injecting
// the signal before temporal/spatial filtering.
bool Renderer::RunRtRestirApplyBeauty(
    CommandList& cl,
    uint32_t frameIndex,
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    const RtDebugRouting& rtDebug)
{
    if (!m_rtEnableRestirEnvDi ||
        !m_rtRestirResolveToBeauty ||
        rtDebug.wantsRtRestirResolveDebug ||
        !m_rtRestirResolvedValidThisFrame ||
        !m_rtRestirAppliedReady ||
        !m_rtRestirResolvedDiffuse ||
        !m_rtRestirResolvedSpec ||
        !m_rtRestirAppliedDiffuse ||
        !m_rtRestirAppliedSpec ||
        !m_rtPostReady ||
        !m_rtPostDiffuse ||
        !m_rtPostSpec)
    {
        return false;
    }

    auto* cmdList = cl.Get();

    CmdBeginEvent(cmdList, "RT ReSTIR Apply Beauty");

    cl.Transition(m_rtPostDiffuse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtPostSpec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtRestirResolvedDiffuse.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl.Transition(m_rtRestirResolvedSpec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.Transition(m_rtRestirAppliedDiffuse.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl.Transition(m_rtRestirAppliedSpec.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cl.FlushBarriers();

    const bool okTables =
        UpdateRtRestirApplyTables(
            frameIndex,
            device,
            m_rtPostDiffuse.Get(),
            m_rtPostSpec.Get(),
            m_rtRestirResolvedDiffuse.Get(),
            m_rtRestirResolvedSpec.Get(),
            m_rtRestirAppliedDiffuse.Get(),
            m_rtRestirAppliedSpec.Get());

    if (!okTables)
    {
        CmdEndEvent(cmdList);
        return false;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS applyCb =
        UpdateRtRestirApplyConstants(frameIndex);

    m_rtRestirApplyPass.Dispatch(
        cl,
        applyCb,
        m_rtFrames[frameIndex].restirApplySrvTable.gpu,
        m_rtFrames[frameIndex].restirApplyUavTable.gpu,
        width,
        height);

    D3D12_RESOURCE_BARRIER barriers[2]{};

    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].UAV.pResource = m_rtRestirAppliedDiffuse.Get();

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[1].UAV.pResource = m_rtRestirAppliedSpec.Get();

    cmdList->ResourceBarrier(2, barriers);

    CmdEndEvent(cmdList);

    return CombineRtSignalsToOutput(
        device,
        cl,
        frameIndex,
        m_rtRestirAppliedDiffuse.Get(),
        m_rtRestirAppliedSpec.Get(),
        width,
        height,
        "RT ReSTIR Applied Combine",
        true);
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::UpdateRtRestirApplyConstants(uint32_t frameIndex)
{
    constexpr uint32_t cbSize =
        (sizeof(RtRestirApplyConstants) + 255u) & ~255u;

    auto alloc = m_upload.Allocate(frameIndex, cbSize, 256);
    auto* cb = reinterpret_cast<RtRestirApplyConstants*>(alloc.cpu);

    *cb = {};
    cb->diffuseScale = std::max(0.0f, m_rtRestirApplyDiffuseScale);
    cb->specularScale = std::max(0.0f, m_rtRestirApplySpecularScale);
    cb->mode = m_rtRestirApplyMode;
    cb->flags = 0;

    return alloc.gpu;
}

void Renderer::ConfigureDefaultPointLights()
{
    m_sceneData.pointLightCount = 3;

    m_sceneData.pointLights[0] =
    {
        { -2.25f, 2.5f, -1.75f },
        10.0f,
        { 1.0f, 0.45f, 0.20f },
        40.0f
    };

    m_sceneData.pointLights[1] =
    {
        { 2.25f, 2.25f, 1.75f },
        10.0f,
        { 0.25f, 0.45f, 1.0f },
        40.0f
    };

    m_sceneData.pointLights[2] =
    {
        { 0.0f, 4.0f, 2.75f },
        12.0f,
        { 0.45f, 1.0f, 0.45f },
        30.0f
    };

    for (uint32_t i = m_sceneData.pointLightCount; i < kMaxPointLights; ++i)
    {
        m_sceneData.pointLights[i] = {};
    }
}

void Renderer::LoadDefaultGltfScene(
    ID3D12Device* device,
    CommandList& cl,
    uint32_t frameIndex)
{
    if (m_importedModelLoadAttempted ||
        m_importedModel.IsLoaded())
    {
        return;
    }

    m_importedModelLoadAttempted = true;

    const std::filesystem::path contentDir =
        Paths::ContentDir_DevOnly();

    if (contentDir.empty())
    {
        OutputDebugStringW(L"glTF scene skipped: content dir is empty.\n");
        return;
    }

    const std::filesystem::path modelPath =
        contentDir /
        L"Models" /
        L"Racetrack" /
        L"racetrackentire.gltf";

    if (!std::filesystem::exists(modelPath))
    {
        OutputDebugStringW(
            (L"glTF scene not found: " + modelPath.wstring() + L"\n").c_str());

        return;
    }

    OutputDebugStringW(
        (L"Loading glTF scene: " + modelPath.wstring() + L"\n").c_str());

    const DirectX::XMMATRIX importTransform =
        DirectX::XMMatrixTranslation(
            0.0f,
            kDefaultImportedModelYOffset,
            0.0f);


    try
    {
        if (!m_importedModel.LoadGltf(
            device,
            cl,
            m_assetUpload,
            m_srvHeap,
            frameIndex,
            modelPath,
            importTransform))
        {
            OutputDebugStringW(
                (L"glTF scene load failed: " +
                    m_importedModel.LastError() +
                    L"\n").c_str());

            return;
        }

        AdoptImportedModelBounds();

        if (!m_importedModelSummaryLogged)
        {
            LogImportedModelSummary();
            m_importedModelSummaryLogged = true;
        }

        OutputDebugStringW(L"glTF scene loaded successfully.\n");
    }
    catch (const std::exception& e)
    {
        // Do not call m_importedModel.Clear() here.
        // The loader may have recorded copy commands referencing partially
        // created resources. Keeping the object alive avoids releasing those
        // resources before the command list has finished executing.
        OutputDebugStringA("glTF scene load threw std::exception: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
    }
    catch (...)
    {
        // Same reason: do not Clear() here.
        OutputDebugStringW(L"glTF scene load threw unknown exception.\n");
    }
}

void Renderer::AppendLoadedModelDraws()
{
    if (!m_importedModelEnabled ||
        !m_importedModel.IsLoaded())
    {
        return;
    }

    Mesh& mesh =
        m_importedModel.GetMesh();

    const std::vector<LoadedModel::Draw>& modelDraws =
        m_importedModel.Draws();

    for (size_t drawIndex = 0; drawIndex < modelDraws.size(); ++drawIndex)
    {
        const LoadedModel::Draw& modelDraw =
            modelDraws[drawIndex];

        Material* material =
            m_importedModel.GetMaterial(modelDraw.materialIndex);

        if (!material)
            continue;

        DrawItem item{};
        item.mesh = &mesh;
        item.material = material;
        item.world = modelDraw.world;
        item.submeshIndex = modelDraw.submeshIndex;
        item.rtObjectId =
            kImportedModelRtObjectIdBase +
            static_cast<uint32_t>(drawIndex);

        m_draws.push_back(item);
    }
}

void Renderer::BuildImportedModelBlas(
    ID3D12Device* device,
    CommandList& cl)
{
    if (!device ||
        !m_device5 ||
        m_importedModelBlasBuilt ||
        !m_importedModel.IsLoaded())
    {
        return;
    }

    auto cmd4 =
        GetCommandList4(cl);

    if (!cmd4)
        return;

    Mesh& mesh =
        m_importedModel.GetMesh();

    const uint32_t submeshCount =
        mesh.SubmeshCount();

    if (submeshCount == 0 ||
        !mesh.VertexBufferResource() ||
        !mesh.IndexBufferResource())
    {
        return;
    }

    if (mesh.IndexFormat() != DXGI_FORMAT_R32_UINT)
    {
        OutputDebugStringW(
            L"Imported glTF DXR BLAS skipped: expected R32 index buffer.\n");

        return;
    }

    cl.Transition(
        mesh.VertexBufferResource(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.Transition(
        mesh.IndexBufferResource(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    cl.FlushBarriers();

    m_importedModelBlas.clear();
    m_importedModelBlas.resize(submeshCount);

    for (uint32_t submeshIndex = 0;
        submeshIndex < submeshCount;
        ++submeshIndex)
    {
        const Mesh::Submesh& submesh =
            mesh.GetSubmesh(submeshIndex);

        if (submesh.indexCount == 0)
            continue;

        AccelerationStructure::GeometryDesc geom{};
        geom.vertexBuffer =
            mesh.VertexBufferResource()->GetGPUVirtualAddress();
        geom.vertexCount =
            mesh.VertexCount();
        geom.vertexStride =
            mesh.VertexStride();
        geom.vertexFormat =
            DXGI_FORMAT_R32G32B32_FLOAT;

        geom.indexBuffer =
            mesh.IndexBufferResource()->GetGPUVirtualAddress();
        geom.indexCount =
            submesh.indexCount;
        geom.indexFormat =
            mesh.IndexFormat();
        geom.indexBufferOffsetBytes =
            static_cast<uint64_t>(submesh.indexStart) *
            sizeof(uint32_t);

        geom.opaque = true;

        std::wstring blasName =
            L"BLAS Imported glTF Submesh " +
            std::to_wstring(submeshIndex);

        m_importedModelBlas[submeshIndex].BuildBottomLevel(
            m_device5.Get(),
            cmd4.Get(),
            geom,
            blasName.c_str());
    }

    m_importedModelBlasBuilt = true;

    OutputDebugStringW(L"Imported glTF BLAS built for DXR.\n");
}

bool Renderer::IsImportedModelMesh(const Mesh* mesh) const
{
    return
        m_importedModel.IsLoaded() &&
        mesh == &m_importedModel.GetMesh();
}

uint32_t Renderer::GetRtMeshTypeForDrawItem(
    const DrawItem& item) const
{
    if (item.mesh == &m_floor)
        return kRtMeshTypeFloor;

    if (item.mesh == &m_quad)
        return kRtMeshTypeQuad;

    if (IsImportedModelMesh(item.mesh))
        return kRtMeshTypeImported;

    return UINT32_MAX;
}

uint32_t Renderer::GetRtIndexStartForDrawItem(
    const DrawItem& item) const
{
    if (!item.mesh)
        return 0u;

    if (!IsImportedModelMesh(item.mesh))
        return 0u;

    return item.mesh->GetSubmesh(item.submeshIndex).indexStart;
}

const AccelerationStructure* Renderer::GetBlasForDrawItem(
    const DrawItem& item) const
{
    if (item.mesh == &m_floor)
        return &m_blasFloor;

    if (item.mesh == &m_quad)
        return &m_blasQuad;

    if (IsImportedModelMesh(item.mesh))
    {
        if (!m_importedModelBlasBuilt)
            return nullptr;

        if (item.submeshIndex >= m_importedModelBlas.size())
            return nullptr;

        return &m_importedModelBlas[item.submeshIndex];
    }

    return nullptr;
}

void Renderer::BuildRtDrawItems()
{
    m_rtDrawItems.clear();

    for (const DrawItem& item : m_draws)
    {
        if (!item.mesh || !item.material)
            continue;

        if (GetRtMeshTypeForDrawItem(item) == UINT32_MAX)
            continue;

        const AccelerationStructure* blas =
            GetBlasForDrawItem(item);

        if (!blas || blas->GpuAddress() == 0)
            continue;

        m_rtDrawItems.push_back(&item);
    }
}

void Renderer::BuildRtMaterialTable()
{
    m_rtMaterialTable.clear();
    m_rtMaterialToId.clear();

    for (const DrawItem* item : m_rtDrawItems)
    {
        if (!item || !item->material)
            continue;

        if (m_rtMaterialToId.find(item->material) != m_rtMaterialToId.end())
            continue;

        if (m_rtMaterialTable.size() >= kRtMaxMaterials)
        {
            OutputDebugStringW(
                L"DXR material table is full; additional materials will use material 0.\n");
            continue;
        }

        const uint32_t materialId =
            static_cast<uint32_t>(m_rtMaterialTable.size());

        m_rtMaterialToId.emplace(item->material, materialId);
        m_rtMaterialTable.push_back(item->material);
    }

    if (m_rtMaterialTable.empty())
    {
        // Keep material 0 valid.
        m_rtMaterialTable.push_back(&m_floorMaterial);
        m_rtMaterialToId.emplace(&m_floorMaterial, 0u);
    }
}

uint32_t Renderer::ResolveRtMaterialId(
    const Material* material) const
{
    if (!material)
        return 0u;

    const auto it =
        m_rtMaterialToId.find(material);

    if (it == m_rtMaterialToId.end())
        return 0u;

    return it->second;
}

void Renderer::AdoptImportedModelBounds()
{
    if (!m_importedModel.IsLoaded() ||
        !m_importedModel.HasBounds())
    {
        return;
    }

    const LoadedModel::Bounds& importedBounds =
        m_importedModel.GetBounds();

    DirectX::XMFLOAT3 currentMin =
        SubFloat3(m_sceneBoundsCenter, m_sceneBoundsExtent);

    DirectX::XMFLOAT3 currentMax =
        AddFloat3(m_sceneBoundsCenter, m_sceneBoundsExtent);

    const DirectX::XMFLOAT3 mergedMin =
        MinFloat3(currentMin, importedBounds.min);

    const DirectX::XMFLOAT3 mergedMax =
        MaxFloat3(currentMax, importedBounds.max);

    m_sceneBoundsCenter =
        MulFloat3(
            AddFloat3(mergedMin, mergedMax),
            0.5f);

    m_sceneBoundsExtent =
        MulFloat3(
            SubFloat3(mergedMax, mergedMin),
            0.5f);

    OutputDebugStringW(
        (L"Scene bounds updated from imported glTF. Center=(" +
            std::to_wstring(m_sceneBoundsCenter.x) + L", " +
            std::to_wstring(m_sceneBoundsCenter.y) + L", " +
            std::to_wstring(m_sceneBoundsCenter.z) + L") Extent=(" +
            std::to_wstring(m_sceneBoundsExtent.x) + L", " +
            std::to_wstring(m_sceneBoundsExtent.y) + L", " +
            std::to_wstring(m_sceneBoundsExtent.z) + L")\n").c_str());
}

void Renderer::LogImportedModelSummary() const
{
    if (!m_importedModel.IsLoaded())
        return;

    const LoadedModel::Stats& stats =
        m_importedModel.GetStats();

    std::wstring message =
        L"Imported glTF summary: vertices=" +
        std::to_wstring(stats.vertexCount) +
        L", indices=" +
        std::to_wstring(stats.indexCount) +
        L", triangles=" +
        std::to_wstring(stats.triangleCount) +
        L", submeshes=" +
        std::to_wstring(stats.submeshCount) +
        L", draws=" +
        std::to_wstring(stats.drawCount) +
        L", materials=" +
        std::to_wstring(stats.materialCount) +
        L", textures=" +
        std::to_wstring(stats.textureCount) +
        L"\n";

    OutputDebugStringW(message.c_str());
}

void Renderer::UpdateRtSceneStats()
{
    RtSceneStats stats{};
    stats.drawCount =
        static_cast<uint32_t>(m_rtDrawItems.size());

    stats.materialCount =
        static_cast<uint32_t>(
            std::min<size_t>(
                m_rtMaterialTable.size(),
                kRtMaxMaterials));

    stats.importedBlasCount =
        m_importedModelBlasBuilt
        ? static_cast<uint32_t>(m_importedModelBlas.size())
        : 0u;

    stats.srvTableCount =
        kRtSrvTableCount;

    stats.tlasInstanceCount =
        static_cast<uint32_t>(m_rtDrawItems.size());

    for (const DrawItem* item : m_rtDrawItems)
    {
        if (!item)
            continue;

        if (IsImportedModelMesh(item->mesh))
        {
            ++stats.importedDrawCount;
        }
    }

    m_rtSceneStats = stats;
}

void Renderer::LogRtSceneStatsIfChanged()
{
    const bool changed =
        !m_rtSceneStatsEverLogged ||
        m_rtSceneStats.drawCount != m_rtSceneStatsLogged.drawCount ||
        m_rtSceneStats.importedDrawCount != m_rtSceneStatsLogged.importedDrawCount ||
        m_rtSceneStats.materialCount != m_rtSceneStatsLogged.materialCount ||
        m_rtSceneStats.importedBlasCount != m_rtSceneStatsLogged.importedBlasCount ||
        m_rtSceneStats.srvTableCount != m_rtSceneStatsLogged.srvTableCount ||
        m_rtSceneStats.tlasInstanceCount != m_rtSceneStatsLogged.tlasInstanceCount;

    if (!changed)
        return;

    std::wstring message =
        L"DXR scene stats: draws=" +
        std::to_wstring(m_rtSceneStats.drawCount) +
        L", importedDraws=" +
        std::to_wstring(m_rtSceneStats.importedDrawCount) +
        L", materials=" +
        std::to_wstring(m_rtSceneStats.materialCount) +
        L", importedBLAS=" +
        std::to_wstring(m_rtSceneStats.importedBlasCount) +
        L", tlasInstances=" +
        std::to_wstring(m_rtSceneStats.tlasInstanceCount) +
        L", srvTableDescriptors=" +
        std::to_wstring(m_rtSceneStats.srvTableCount) +
        L"\n";

    OutputDebugStringW(message.c_str());

    m_rtSceneStatsLogged = m_rtSceneStats;
    m_rtSceneStatsEverLogged = true;
}

bool Renderer::ValidateRtSceneContract() const
{
    if (m_rtDrawItems.empty())
        return false;

    if (m_rtMaterialTable.empty())
        return false;

    if (m_rtMaterialTable.size() > kRtMaxMaterials)
        return false;

    for (const DrawItem* item : m_rtDrawItems)
    {
        if (!item || !item->mesh || !item->material)
            return false;

        const uint32_t meshType =
            GetRtMeshTypeForDrawItem(*item);

        if (meshType == UINT32_MAX)
            return false;

        const AccelerationStructure* blas =
            GetBlasForDrawItem(*item);

        if (!blas || blas->GpuAddress() == 0)
            return false;

        const uint32_t materialId =
            ResolveRtMaterialId(item->material);

        if (materialId >= kRtMaxMaterials)
            return false;
    }

    return true;
}

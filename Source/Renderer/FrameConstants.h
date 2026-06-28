#pragma once
#include <DirectXMath.h>
#include <cstdint>

#include "Source/Renderer/SceneData.h"


// Forward PBR - We use row-major matrices: HLSL declares row_major and multiplies mul(vec, mat)
struct PerFrameConstants
{
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT4X4 lightViewProj;

    DirectX::XMFLOAT3   cameraPos;
    float      time;

    uint32_t   frameIndex;
    uint32_t    hasBRDFLut; // 1 if loaded, 0 if null
    uint32_t    hasIBL;     // 1 if diffuse/specular loaded, 0 if null
    uint32_t    _pad0;      // keep one for alignment


    DirectX::XMFLOAT3   lightDir;   float pad1;  
    DirectX::XMFLOAT3   lightColor; float pad2;

    DirectX::XMFLOAT2 shadowInvSize;
    uint32_t debugView = 0;
    uint32_t rtSampleIndex = 0;
    uint32_t rtResetId = 0;
    uint32_t rtAccumulate = 0;
    uint32_t rtEnableIndirect = 1;
    float    rtIndirectScale = 1.0f;

    uint32_t pointLightCount = 0;
    DirectX::XMFLOAT3 pointLightPad{};

    PointLight pointLights[kMaxPointLights]{};

    float iblIntensity = 1.0f;
    float iblRotationRadians = 0.0f;
    uint32_t hasLightingEnvironment = 0;
    uint32_t _padIbl = 0;
};
static_assert(sizeof(PointLight) == 32, "PointLight must match HLSL PointLightData layout.");
static_assert((sizeof(PerFrameConstants) % 16) == 0); 

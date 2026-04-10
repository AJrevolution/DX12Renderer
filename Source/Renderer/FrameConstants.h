#pragma once
#include <DirectXMath.h>

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
    uint32_t padShadow = 0;
};
static_assert((sizeof(PerFrameConstants) % 16) == 0); 

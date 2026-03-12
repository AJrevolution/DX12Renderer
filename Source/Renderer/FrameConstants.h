#pragma once
#include <DirectXMath.h>

// Forward PBR - We use row-major matrices: HLSL declares row_major and multiplies mul(vec, mat)
struct PerFrameConstants
{
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3   cameraPos;
    float      time;
    uint32_t   frameIndex;
    float      padding[3]; 


    DirectX::XMFLOAT3   lightDir;   float pad1;  
    DirectX::XMFLOAT3   lightColor; float pad2;
};
static_assert((sizeof(PerFrameConstants) % 16) == 0); 

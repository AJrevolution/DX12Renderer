#pragma once
#include <DirectXMath.h>


struct PerFrameConstants
{
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT3   cameraPos;
    float      time;
    uint32_t   frameIndex;
    float      padding[43]; //keep alignment
};
static_assert((sizeof(PerFrameConstants) % 256) == 0, "CBV size must be 256-byte aligned!");

#pragma once
#include <DirectXMath.h>
using namespace DirectX;

struct PerDrawConstants
{
    XMFLOAT4X4 world;
    uint32_t materialIndex; // reserved for future material buffer indexing
    uint32_t pad[3];
};
static_assert((sizeof(PerDrawConstants) % 16) == 0);

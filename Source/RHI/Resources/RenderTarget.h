#pragma once
#include "Common.h"
#include "Source/RHI/Resources/Texture.h"
#include "Source/RHI/Memory/DescriptorAllocator.h"

class RenderTarget
{
public:
    void Create(
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        const float clearColor[4],
        const wchar_t* name);


    Texture& Tex() { return m_tex; }
    const Texture& Tex() const { return m_tex; }

    D3D12_CPU_DESCRIPTOR_HANDLE RTV() const { return m_rtv; }
    bool HasRTV() const { return m_rtv.ptr != 0; }

    const float* ClearColor() const { return m_clear; }

private:
    Texture m_tex;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
    float m_clear[4]{};
};

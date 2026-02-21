#pragma once
#include "Common.h"

class Texture
{
public:
    void CreateDepth(
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT resourceFormat, // typeless (recommended): DXGI_FORMAT_R32_TYPELESS
        DXGI_FORMAT dsvFormat,      // view: DXGI_FORMAT_D32_FLOAT
        const wchar_t* name);
    
    void Create2D(ID3D12Device* device, 
        uint32_t width, 
        uint32_t height, 
        DXGI_FORMAT format, 
        const wchar_t* name);

    ID3D12Resource* Get() const { return m_resource.Get(); }
    DXGI_FORMAT ResourceFormat() const { return m_resourceFormat; }

private:
    ComPtr<ID3D12Resource> m_resource;
    DXGI_FORMAT m_resourceFormat = DXGI_FORMAT_UNKNOWN;
};

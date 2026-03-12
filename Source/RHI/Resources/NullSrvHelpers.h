#pragma once
#include "Common.h"

inline void CreateNullTexture2DSRV(
    ID3D12Device* device,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Texture2D.MostDetailedMip = 0;

    device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
}

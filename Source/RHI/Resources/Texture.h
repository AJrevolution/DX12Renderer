#pragma once
#include "Common.h"
#include "Source/RHI/Memory/UploadArena.h"
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include <filesystem>
#include "Source/RHI/CommandList/CommandList.h"

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

    void LoadFromFile_DirectXTex(
        ID3D12Device* device,
        CommandList& cl,
        UploadArena& upload,
        uint32_t frameIndex,
        const std::filesystem::path& filePath,
        bool treatAsSRGB,
        const wchar_t* debugName);

    void CreateRenderTarget(
        ID3D12Device* device,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        const float clearColor[4],
        const wchar_t* name);

    ID3D12Resource* Get() const { return m_resource.Get(); }
    DXGI_FORMAT ResourceFormat() const { return m_resourceFormat; }
    DXGI_FORMAT SrvFormat() const { return m_srvFormat; }

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t MipCount() const { return m_mipCount; }

    bool IsValid() const { return m_resource != nullptr; }

private:
    ComPtr<ID3D12Resource> m_resource;
    DXGI_FORMAT m_srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_resourceFormat = DXGI_FORMAT_UNKNOWN;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_mipCount = 0;
};

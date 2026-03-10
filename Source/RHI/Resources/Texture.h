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

    ID3D12Resource* Get() const { return m_resource.Get(); }
    DXGI_FORMAT ResourceFormat() const { return m_resourceFormat; }
    DXGI_FORMAT SrvFormat() const { return m_srvFormat; }
private:
    ComPtr<ID3D12Resource> m_resource;
    DXGI_FORMAT m_srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_resourceFormat = DXGI_FORMAT_UNKNOWN;
};

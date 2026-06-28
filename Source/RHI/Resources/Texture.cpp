#include "Texture.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include <DirectXTex.h>
#include <cstdio>
#include <sstream>

namespace
{
    std::wstring ToWideString(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::string HResultToString(HRESULT hr)
    {
        char buffer[32]{};
        sprintf_s(
            buffer,
            "0x%08X",
            static_cast<unsigned int>(static_cast<uint32_t>(hr)));

        return buffer;
    }
}

void Texture::Reset()
{
    m_resource.Reset();

    m_srvFormat = DXGI_FORMAT_UNKNOWN;
    m_resourceFormat = DXGI_FORMAT_UNKNOWN;
    m_dsvFormat = DXGI_FORMAT_UNKNOWN;

    m_width = 0;
    m_height = 0;
    m_mipCount = 0;

    m_dimension = TextureDimension::Unknown;
    m_arraySize = 0;
}

bool Texture::TryLoadFromFile_DirectXTex(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::filesystem::path& filePath,
    bool treatAsSRGB,
    const wchar_t* debugName,
    std::wstring* errorOut)
{
    try
    {
        LoadFromFile_DirectXTex(
            device,
            cl,
            upload,
            frameIndex,
            filePath,
            treatAsSRGB,
            debugName);

        return IsValid();
    }
    catch (const std::exception& e)
    {
        Reset();

        if (errorOut)
        {
            *errorOut =
                L"Texture load failed: " +
                filePath.wstring() +
                L" : " +
                ToWideString(e.what());
        }

        return false;
    }
    catch (...)
    {
        Reset();

        if (errorOut)
        {
            *errorOut =
                L"Texture load failed with unknown exception: " +
                filePath.wstring();
        }

        return false;
    }
}

void Texture::CreateDepth(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT resourceFormat,
    DXGI_FORMAT dsvFormat,
    const wchar_t* name)
{
    m_width = width;
    m_height = height;
    m_mipCount = 1;
    m_resourceFormat = resourceFormat;
    m_dsvFormat = dsvFormat;
    m_srvFormat = DXGI_FORMAT_R32_FLOAT;
    m_dimension = TextureDimension::Texture2D;
    m_arraySize = 1;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = dsvFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = resourceFormat; // typeless
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear,
            IID_PPV_ARGS(&m_resource)),
        "CreateCommittedResource(Depth)"
    );

    CommandList::SetGlobalState(m_resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (name)
        SetD3D12ObjectName(m_resource.Get(), name);
}

void Texture::Create2D(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    m_width = width;
    m_height = height;
    m_mipCount = 1;
    m_resourceFormat = format;
    m_srvFormat = format;

    m_dimension = TextureDimension::Texture2D;
    m_arraySize = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, // Created in Copy Dest for initial upload
        nullptr,
        IID_PPV_ARGS(&m_resource)
    ), "Create2D Texture Resource");

    if (name) m_resource->SetName(name);
}
static DXGI_FORMAT MakeSRGBIfNeeded(DXGI_FORMAT fmt, bool srgb)
{
    if (!srgb) return fmt;
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return fmt; // keep as-is if no SRGB variant
    }
}


void Texture::LoadFromFile_DirectXTex(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::filesystem::path& filePath,
    bool treatAsSRGB,
    const wchar_t* debugName)
{
    Reset();

    if (!device)
        throw std::runtime_error("Texture::LoadFromFile_DirectXTex: null device.");

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec))
    {
        throw std::runtime_error(
            "Texture::LoadFromFile_DirectXTex: file does not exist: " +
            filePath.string());
    }

    using namespace DirectX;

    ScratchImage image;
    TexMetadata meta{};

    const std::wstring wpath = filePath.wstring();
    HRESULT hr = S_OK;

    // Load the raw data (WIC handles PNG/JPG/BMP/TIF)
    if (filePath.extension() == L".dds" || filePath.extension() == L".DDS")
        hr = LoadFromDDSFile(wpath.c_str(), DDS_FLAGS_NONE, &meta, image);
    else
        hr = LoadFromWICFile(wpath.c_str(), WIC_FLAGS_NONE, &meta, image);

    if (FAILED(hr))
    {
        throw std::runtime_error(
            "DirectXTex: failed to load file: " +
            filePath.string() +
            " hr=" +
            HResultToString(hr));
    }

    if (image.GetImageCount() == 0)
    {
        throw std::runtime_error(
            "DirectXTex: loaded image has no images: " +
            filePath.string());
    }

    // 1. If it's compressed (BC1-BC7), we must decompress it first to convert it
    if (DirectX::IsCompressed(meta.format))
    {
        ScratchImage decompressed;
        hr = Decompress(image.GetImages(), image.GetImageCount(), meta, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
        
        if (FAILED(hr))
        {
            throw std::runtime_error(
                "DirectXTex: failed to decompress texture: " +
                filePath.string() +
                " hr=" +
                HResultToString(hr));
        }


        image = std::move(decompressed);
        meta = image.GetMetadata();
    }
    const Image* src = image.GetImage(0, 0, 0);
    // BODGE: Force conversion to R8G8B8A8_UNORM for now
	//TODO add either separate function for other types of resources like HDR or add support for more formats in this function.
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {

        ScratchImage converted;
        hr = Convert(
            image.GetImages(), image.GetImageCount(), meta,
            DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT,
            converted
        );
        
        if (FAILED(hr))
        {
            throw std::runtime_error(
                "DirectXTex: failed to convert texture to RGBA8: " +
                filePath.string() +
                " hr=" +
                HResultToString(hr));
        }

        image = std::move(converted);
        //meta.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        meta = image.GetMetadata();
  
    }

    src = image.GetImage(0, 0, 0);
    if (!src || !src->pixels)
        throw std::runtime_error("DirectXTex: missing image pixels.");

    m_width = static_cast<uint32_t>(src->width);
    m_height = static_cast<uint32_t>(src->height);
    m_mipCount = 1;
    m_resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_srvFormat = MakeSRGBIfNeeded(m_resourceFormat, treatAsSRGB);
    m_dimension = TextureDimension::Texture2D;
    m_arraySize = 1;

    // Create Default Resource
    D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, m_width, m_height);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource)
    ), "Texture creation failed");
    
    CommandList::SetGlobalState(m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    
    m_resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (debugName) m_resource->SetName(debugName);

    // Upload Logic using Footprints
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    // Allocate from Arena 
    auto uploadAlloc = upload.Allocate(frameIndex, totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Copy pixels to upload buffer
    for (UINT y = 0; y < numRows; ++y)
    {
        memcpy(
            uploadAlloc.cpu + (y * footprint.Footprint.RowPitch),
            src->pixels + (y * src->rowPitch),
            static_cast<size_t>(rowSize));
    }

    // Execute Copy
    auto dstLoc = CD3DX12_TEXTURE_COPY_LOCATION(m_resource.Get(), 0);
    auto srcLoc = CD3DX12_TEXTURE_COPY_LOCATION(upload.GetBuffer(frameIndex), footprint);
    srcLoc.PlacedFootprint.Offset = uploadAlloc.offset;

    cl.CopyTexture(dstLoc, 0, 0, 0, srcLoc, nullptr);

    // Transition to Shader Resource
    cl.Transition(m_resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);    

    return;
}

void Texture::CreateRenderTarget(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    const float clearColor[4],
    const wchar_t* name)
{
    m_width = width;
    m_height = height;
    m_mipCount = 1;
    m_resourceFormat = format;
    m_srvFormat = format;

    m_dimension = TextureDimension::Texture2D;
    m_arraySize = 1;

    //create heap 
    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1);
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    clear.Color[0] = clearColor[0];
    clear.Color[1] = clearColor[1];
    clear.Color[2] = clearColor[2];
    clear.Color[3] = clearColor[3];

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clear,
            IID_PPV_ARGS(&m_resource)),
        "CreateCommittedResource(RenderTarget)"
    );

    CommandList::SetGlobalState(m_resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (name)
        SetD3D12ObjectName(m_resource.Get(), name);
}

void Texture::CreateFromRGBA8Data(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    uint32_t width,
    uint32_t height,
    const void* rgba8Pixels,
    bool treatAsSRGB,
    const wchar_t* debugName)
{
    if (!rgba8Pixels || width == 0 || height == 0)
        throw std::runtime_error("CreateFromRGBA8Data: invalid texture data.");

    m_width = width;
    m_height = height;
    m_mipCount = 1;
    m_resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_srvFormat = MakeSRGBIfNeeded(m_resourceFormat, treatAsSRGB);
    m_dsvFormat = DXGI_FORMAT_UNKNOWN;

    m_dimension = TextureDimension::Texture2D;
    m_arraySize = 1;

    D3D12_RESOURCE_DESC texDesc =
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height);

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_resource)),
        "CreateFromRGBA8Data: texture creation failed");

    CommandList::SetGlobalState(m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

    if (debugName)
        m_resource->SetName(debugName);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    auto uploadAlloc = upload.Allocate(frameIndex, totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    const uint8_t* src = reinterpret_cast<const uint8_t*>(rgba8Pixels);
    const size_t srcRowPitch = static_cast<size_t>(width) * 4u;

    for (UINT y = 0; y < numRows; ++y)
    {
        memcpy(
            uploadAlloc.cpu + (y * footprint.Footprint.RowPitch),
            src + (y * srcRowPitch),
            srcRowPitch);
    }

    auto dstLoc = CD3DX12_TEXTURE_COPY_LOCATION(m_resource.Get(), 0);
    auto srcLoc = CD3DX12_TEXTURE_COPY_LOCATION(upload.GetBuffer(frameIndex), footprint);
    srcLoc.PlacedFootprint.Offset = uploadAlloc.offset;

    cl.CopyTexture(dstLoc, 0, 0, 0, srcLoc, nullptr);
    cl.Transition(m_resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

D3D12_SHADER_RESOURCE_VIEW_DESC Texture::MakeSrvDesc() const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = m_srvFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (m_dimension == TextureDimension::TextureCube)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels = m_mipCount;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = m_mipCount;
        srv.Texture2D.PlaneSlice = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
    }

    return srv;
}

D3D12_SHADER_RESOURCE_VIEW_DESC Texture::MakeCubeSrvDesc() const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = m_srvFormat;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MostDetailedMip = 0;
    srv.TextureCube.MipLevels = m_mipCount;
    srv.TextureCube.ResourceMinLODClamp = 0.0f;
    return srv;
}

bool Texture::TryLoadCubeFromDDS_DirectXTex(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::filesystem::path& filePath,
    bool treatAsSRGB,
    const wchar_t* debugName,
    std::wstring* errorOut)
{
    try
    {
        LoadCubeFromDDS_DirectXTex(
            device,
            cl,
            upload,
            frameIndex,
            filePath,
            treatAsSRGB,
            debugName);

        return IsValid() && IsCube();
    }
    catch (const std::exception& e)
    {
        Reset();

        if (errorOut)
        {
            *errorOut =
                L"Cubemap load failed: " +
                filePath.wstring() +
                L" : " +
                ToWideString(e.what());
        }

        return false;
    }
    catch (...)
    {
        Reset();

        if (errorOut)
        {
            *errorOut =
                L"Cubemap load failed with unknown exception: " +
                filePath.wstring();
        }

        return false;
    }
}
void Texture::LoadCubeFromDDS_DirectXTex(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::filesystem::path& filePath,
    bool treatAsSRGB,
    const wchar_t* debugName)
{
    Reset();

    if (!device)
        throw std::runtime_error("Texture::LoadCubeFromDDS_DirectXTex: null device.");

    std::error_code ec;

    if (!std::filesystem::exists(filePath, ec))
    {
        throw std::runtime_error(
            "Texture::LoadCubeFromDDS_DirectXTex: file does not exist: " +
            filePath.string());
    }

    const std::wstring wpath =
        filePath.wstring();

    DirectX::ScratchImage image;
    DirectX::TexMetadata meta{};

    HRESULT hr =
        DirectX::LoadFromDDSFile(
            wpath.c_str(),
            DirectX::DDS_FLAGS_NONE,
            &meta,
            image);

    if (FAILED(hr))
    {
        throw std::runtime_error(
            "DirectXTex: failed to load cubemap DDS: " +
            filePath.string() +
            " hr=" +
            HResultToString(hr));
    }

    if (!meta.IsCubemap())
    {
        throw std::runtime_error(
            "DirectXTex: DDS is not a cubemap: " +
            filePath.string());
    }

    if (meta.arraySize != 6)
    {
        throw std::runtime_error(
            "DirectXTex: cubemap DDS must contain exactly 6 faces for this phase: " +
            filePath.string());
    }

    if (image.GetImageCount() == 0)
    {
        throw std::runtime_error(
            "DirectXTex: cubemap has no image data: " +
            filePath.string());
    }

    const size_t mipCount =
        std::max<size_t>(1, meta.mipLevels);

    for (size_t face = 0; face < 6; ++face)
    {
        for (size_t mip = 0; mip < mipCount; ++mip)
        {
            const DirectX::Image* src =
                image.GetImage(
                    mip,
                    face,
                    0);

            if (!src || !src->pixels)
            {
                throw std::runtime_error(
                    "DirectXTex: missing cubemap face/mip data before upload: " +
                    filePath.string());
            }
        }
    }

    DXGI_FORMAT resourceFormat = meta.format;

    if (resourceFormat == DXGI_FORMAT_UNKNOWN)
    {
        throw std::runtime_error(
            "DirectXTex: cubemap has unknown format: " +
            filePath.string());
    }

    m_width =
        static_cast<uint32_t>(meta.width);

    m_height =
        static_cast<uint32_t>(meta.height);

    m_mipCount =
        static_cast<uint32_t>(std::max<size_t>(1, meta.mipLevels));

    m_arraySize = 6;
    m_dimension = TextureDimension::TextureCube;

    m_resourceFormat = resourceFormat;
    m_srvFormat = MakeSRGBIfNeeded(resourceFormat, treatAsSRGB);
    m_dsvFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = meta.width;
    texDesc.Height = static_cast<UINT>(meta.height);
    texDesc.DepthOrArraySize = 6;
    texDesc.MipLevels = static_cast<UINT16>(m_mipCount);
    texDesc.Format = resourceFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto defaultHeap =
        CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_resource)),
        "Texture::LoadCubeFromDDS_DirectXTex: CreateCommittedResource failed");

    CommandList::SetGlobalState(
        m_resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST);

    if (debugName)
        m_resource->SetName(debugName);

    const UINT subresourceCount =
        static_cast<UINT>(m_arraySize * m_mipCount);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
    std::vector<UINT> numRows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);

    UINT64 totalBytes = 0;

    device->GetCopyableFootprints(
        &texDesc,
        0,
        subresourceCount,
        0,
        layouts.data(),
        numRows.data(),
        rowSizes.data(),
        &totalBytes);

    auto uploadAlloc =
        upload.Allocate(
            frameIndex,
            totalBytes,
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    for (UINT face = 0; face < 6; ++face)
    {
        for (UINT mip = 0; mip < m_mipCount; ++mip)
        {
            const UINT subresource =
                mip + face * m_mipCount;

            const DirectX::Image* src =
                image.GetImage(
                    mip,
                    face,
                    0);

            if (!src || !src->pixels)
            {
                throw std::runtime_error(
                    "DirectXTex: missing cubemap face/mip data: " +
                    filePath.string());
            }

            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout =
                layouts[subresource];

            for (UINT row = 0; row < numRows[subresource]; ++row)
            {
                std::memcpy(
                    uploadAlloc.cpu +
                    layout.Offset +
                    static_cast<size_t>(row) * layout.Footprint.RowPitch,
                    src->pixels +
                    static_cast<size_t>(row) * src->rowPitch,
                    static_cast<size_t>(rowSizes[subresource]));
            }

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = m_resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = subresource;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = upload.GetBuffer(frameIndex);
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = layout;
            srcLoc.PlacedFootprint.Offset += uploadAlloc.offset;

            cl.CopyTexture(
                dst,
                0,
                0,
                0,
                srcLoc,
                nullptr);
        }
    }

    cl.Transition(
        m_resource.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}


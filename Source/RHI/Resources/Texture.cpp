#include "Texture.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include <DirectXTex.h>

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
    
    const Image* src = image.GetImage(0, 0, 0);
    // BODGE: Force conversion to R8G8B8A8_UNORM for now
	//TODO add either separate function for other types of resources like HDR or add support for more formats in this function.
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        // Check if the format is compressed (BC1-BC7)
        if (DirectX::IsCompressed(meta.format))
        {
            // For now don't support loading compressed textures into this 8-bit path.
            // fix later: call DirectX::Decompress() here.
            throw std::runtime_error("DirectXTex: Compressed DDS not supported in this 8-bit bodge path.");
        }
        ScratchImage converted;
        hr = Convert(
            image.GetImages(), image.GetImageCount(), meta,
            DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT,
            converted
        );
        
        ThrowIfFailed(hr, "DirectXTex: Failed to convert/downsample to RGBA8");

        image = std::move(converted);
        meta.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src = image.GetImage(0, 0, 0);
    }

   
    if (!src || !src->pixels)
        throw std::runtime_error("DirectXTex: missing image pixels.");

    m_width = static_cast<uint32_t>(src->width);
    m_height = static_cast<uint32_t>(src->height);
    m_mipCount = 1;
    m_resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_srvFormat = MakeSRGBIfNeeded(m_resourceFormat, treatAsSRGB);

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

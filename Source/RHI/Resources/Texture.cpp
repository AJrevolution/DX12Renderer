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
    m_resourceFormat = resourceFormat;

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

    if (name)
        SetD3D12ObjectName(m_resource.Get(), name);
}

void Texture::Create2D(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    m_resourceFormat = format;

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


DescriptorAllocator::Allocation Texture::LoadFromFile_DirectXTex(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmd,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::filesystem::path& filePath,
    DescriptorAllocator& srvHeap,
    bool treatAsSRGB,
    const wchar_t* debugName)
{
    using namespace DirectX;

    ScratchImage image;
    TexMetadata meta{};

    const std::wstring wpath = filePath.wstring();
    HRESULT hr = S_OK;

    // Load the raw data 
    if (filePath.extension() == L".dds" || filePath.extension() == L".DDS")
        hr = LoadFromDDSFile(wpath.c_str(), DDS_FLAGS_NONE, &meta, image);
    else
        hr = LoadFromWICFile(wpath.c_str(), WIC_FLAGS_NONE, &meta, image);

    // Ensure we have R8G8B8A8_UNORM for simplicity
    //// TODO: support Mip chains, BC compression, and native formats.
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        ScratchImage converted;
        hr = Convert(
            image.GetImages(), image.GetImageCount(), meta,
            DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT,
            converted
        );
        ThrowIfFailed(hr, "DirectXTex: Failed to convert to RGBA8");

        image = std::move(converted);
        meta.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // force RGBA8 for simplicity
    const Image* src = image.GetImage(0, 0, 0);

    const uint32_t width = (uint32_t)src->width;
    const uint32_t height = (uint32_t)src->height;

    // Determine SRV format based on sRGB intent
    DXGI_FORMAT srvFormat = treatAsSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    
    // Create Default Resource
    D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height);
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_resource)
    ), "Texture creation failed");

    m_resourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (debugName) m_resource->SetName(debugName);

    // Upload Logic using Footprints
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0, totalBytes = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    // Allocate from Arena (Ensuring we bumped the size to 16MB+ in Renderer!)
    auto uploadAlloc = upload.Allocate(frameIndex, totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Copy pixels to upload buffer
    for (UINT y = 0; y < numRows; ++y)
    {
        memcpy(uploadAlloc.cpu + (y * footprint.Footprint.RowPitch),
            src->pixels + (y * src->rowPitch), rowSize);
    }

    // Execute Copy
    auto dstLoc = CD3DX12_TEXTURE_COPY_LOCATION(m_resource.Get(), 0);
    auto srcLoc = CD3DX12_TEXTURE_COPY_LOCATION(upload.GetBuffer(frameIndex), footprint);
    srcLoc.PlacedFootprint.Offset = uploadAlloc.offset;

    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition to Shader Resource
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);

    // INTERNAL SRV ALLOCATION 
    auto allocation = srvHeap.Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    device->CreateShaderResourceView(m_resource.Get(), &srvDesc, allocation.cpu);

    return allocation;
}
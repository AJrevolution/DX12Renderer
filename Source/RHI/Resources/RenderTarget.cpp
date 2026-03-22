#include "Source/RHI/Resources/RenderTarget.h"

void RenderTarget::Create(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    const float clearColor[4],
    const wchar_t* name)
{
    memcpy(m_clear, clearColor, sizeof(float) * 4);
    m_tex.CreateRenderTarget(device, width, height, format, m_clear, name);
}

void RenderTarget::CreateRTV(
    ID3D12Device* device,
    DescriptorAllocator& rtvHeap,
    const wchar_t* rtvName)
{
    if (!m_rtv.ptr)
    {
        auto alloc = rtvHeap.Allocate(1);
        m_rtv = alloc.cpu;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = m_tex.ResourceFormat();
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    device->CreateRenderTargetView(m_tex.Get(), &rtv, m_rtv);

    if (rtvName)
        SetD3D12ObjectName(m_tex.Get(), rtvName);
}

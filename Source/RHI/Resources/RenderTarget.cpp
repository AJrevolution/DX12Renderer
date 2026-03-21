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


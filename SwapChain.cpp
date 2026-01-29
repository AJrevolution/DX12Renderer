#include "SwapChain.h"

static bool CheckTearingSupport(IDXGIFactory7* factory)
{
    BOOL allowTearing = FALSE;
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5))))
    {
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
            return allowTearing == TRUE;
    }
    return false;
}

void SwapChain::Initialize(
    IDXGIFactory7* factory,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    HWND hwnd,
    uint32_t width,
    uint32_t height,
    uint32_t bufferCount,
    DXGI_FORMAT format)
{
    m_width = width;
    m_height = height;
    m_bufferCount = bufferCount;
    m_format = format;
    m_allowTearing = CheckTearingSupport(factory);

    // RTV heap for backbuffers
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = bufferCount;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV heap");

    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = format;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = bufferCount;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.Stereo = FALSE;
    scDesc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(queue, hwnd, &scDesc, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
    ThrowIfFailed(sc1.As(&m_swapChain), "SwapChain1->SwapChain4");

    // Disable Alt+Enter fullscreen toggling (manage fullscreen later)
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    CreateBackBuffers(device);
}

void SwapChain::CreateBackBuffers(ID3D12Device* device)
{
    m_backBuffers.resize(m_bufferCount);

    auto rtvStart = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < m_bufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])), "GetBuffer(backbuffer)");
        std::wstring name = std::format(L"BackBuffer {}", i);
        SetD3D12ObjectName(m_backBuffers[i].Get(), name.c_str());

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvStart;
        rtv.ptr += (SIZE_T)i * (SIZE_T)m_rtvDescriptorSize;
        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
    }

    m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void SwapChain::ReleaseBackBuffers()
{
    for (auto& bb : m_backBuffers) bb.Reset();
    m_backBuffers.clear();
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::GetCurrentRTV() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)m_currentIndex * (SIZE_T)m_rtvDescriptorSize;
    return h;
}

void SwapChain::Present(bool vsync)
{
    // vsync: sync interval 1; tearing only when vsync off
    const UINT syncInterval = vsync ? 1 : 0;
    const UINT presentFlags = (!vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;

    ThrowIfFailed(m_swapChain->Present(syncInterval, presentFlags), "Present");
    m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void SwapChain::Resize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    ReleaseBackBuffers();

    UINT flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ThrowIfFailed(m_swapChain->ResizeBuffers(m_bufferCount, width, height, m_format, flags), "ResizeBuffers");

    CreateBackBuffers(device);
}

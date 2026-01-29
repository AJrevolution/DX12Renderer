#pragma once
#include "Common.h"
#include <vector>

class SwapChain
{
public:
    void Initialize(
        IDXGIFactory7* factory,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        HWND hwnd,
        uint32_t width,
        uint32_t height,
        uint32_t bufferCount = 2,
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM
    );

    void Present(bool vsync = true);

    void Resize(ID3D12Device* device, uint32_t width, uint32_t height);

    uint32_t GetCurrentBackBufferIndex() const { return m_currentIndex; }
    ID3D12Resource* GetCurrentBackBuffer() const { return m_backBuffers[m_currentIndex].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() const;

    DXGI_FORMAT GetFormat() const { return m_format; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    uint32_t BufferCount() const { return m_bufferCount; }
private:
    void CreateBackBuffers(ID3D12Device* device);
    void ReleaseBackBuffers();

private:
    ComPtr<IDXGISwapChain4> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    std::vector<ComPtr<ID3D12Resource>> m_backBuffers;

    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t m_bufferCount = 2;
    uint32_t m_rtvDescriptorSize = 0;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_currentIndex = 0;

    bool m_allowTearing = false;
};

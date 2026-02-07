#pragma once
#include "Common.h"
#include "Window.h"
#include "Device.h"
#include "CommandQueue.h"
#include "SwapChain.h"
#include "GPUMarkers.h"
#include "GPUTimer.h"
#include "Source/Core/Paths.h"

#include <array>
#include <vector>

class Application
{
public:
    bool Initialize(uint32_t width, uint32_t height, const wchar_t* title);
    int Run();

private:
    void Tick();
    void Render();
    void HandleResizeIfNeeded();

    void BeginFrame();
    void EndFrame();

private:
    static constexpr uint32_t kFrameCount = 3;

    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

private:
    Window m_window;

    Device m_device;
    CommandQueue m_graphicsQueue;
    SwapChain m_swapChain;

    std::array<FrameContext, kFrameCount> m_frames{};
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    uint32_t m_frameIndex = 0; // derived from swapchain index each frame

    // Backbuffer state tracking (honest size, no magic 3 mismatch)
    std::vector<D3D12_RESOURCE_STATES> m_backBufferStates;

    float m_clearColor[4] = { 0.08f, 0.10f, 0.14f, 1.0f };

    GPUTimer m_frameTimer;
};

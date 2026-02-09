#include "Application.h"
#include "Source/Renderer/Passes/TrianglePass.h"
#include "Source/Core/Paths.h"

TrianglePass m_triangle;


bool Application::Initialize(uint32_t width, uint32_t height, const wchar_t* title)
{
    //Window
    if (!m_window.Create(width, height, title))
        return false;

    //Device
    m_device.Initialize(false, true);

    //Command Queue
    m_graphicsQueue.Initialize(m_device.GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);

    //Timing
    m_frameTimer.Initialize(m_device.GetDevice());

    //Swap Chain
    m_swapChain.Initialize(
        m_device.GetFactory(),
        m_device.GetDevice(),
        m_graphicsQueue.Get(),
        m_window.GetHWND(),
        width, height,
        kFrameCount,
        DXGI_FORMAT_R8G8B8A8_UNORM
    );


    //Per-frame allocators
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(
            m_device.GetDevice()->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_frames[i].allocator)),
            "CreateCommandAllocator(frame)"
        );
        SetD3D12ObjectName(m_frames[i].allocator.Get(), std::format(L"CmdAlloc Frame {}", i).c_str());
        m_frames[i].fenceValue = 0;
    }

    //One command list reused; reset with the current frame allocator each frame
    ThrowIfFailed(
        m_device.GetDevice()->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frames[0].allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_cmdList)),
        "CreateCommandList"
    );
    SetD3D12ObjectName(m_cmdList.Get(), L"Main Graphics CmdList");

    //Command lists are created in an open state; close until used
    ThrowIfFailed(m_cmdList->Close(), "Close initial command list");

    //Honest backbuffer state tracking
    m_backBufferStates.assign(kFrameCount, D3D12_RESOURCE_STATE_PRESENT);
   
	
    const auto shaderDir = Paths::ExecutableDir() / L"Shaders" / L"Compiled";
    m_triangle.Initialize(m_device.GetDevice(), m_swapChain.GetFormat(), shaderDir);

    return true;
}

int Application::Run()
{
    while (m_window.PumpMessages())
    {
        Tick();
    }

    // Full flush on shutdown 
    m_graphicsQueue.Flush();
    return 0;
}

void Application::Tick()
{
    HandleResizeIfNeeded();
    Render();
}

void Application::HandleResizeIfNeeded()
{
    uint32_t w = 0, h = 0;
    if (!m_window.ConsumeResize(w, h))
        return;

    // IMPORTANT: wait for GPU before resizing swapchain buffers
    m_graphicsQueue.Flush();

    m_swapChain.Resize(m_device.GetDevice(), w, h);

    // Reset state tracking after resize
    m_backBufferStates.assign(m_swapChain.BufferCount(), D3D12_RESOURCE_STATE_PRESENT);
}

void Application::BeginFrame()
{
    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();
    FrameContext& frame = m_frames[m_frameIndex];

    //If GPU is still using this frame’s allocator, wait for its fence
    if (frame.fenceValue != 0)
        m_graphicsQueue.Wait(frame.fenceValue);

    //Reset command allocator
    ThrowIfFailed(frame.allocator->Reset(), "Frame Alloc Reset");
    //Open command list, ready to write new commands
    ThrowIfFailed(m_cmdList->Reset(frame.allocator.Get(), nullptr), "CmdList Reset");
}

void Application::EndFrame()
{
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_graphicsQueue.Get()->ExecuteCommandLists(1, lists);

    m_swapChain.Present(true);

    // Signal AFTER submit; store per-frame fence value
    const uint64_t fence = m_graphicsQueue.Signal();
    m_frames[m_frameIndex].fenceValue = fence;
}

void Application::Render()
{

    BeginFrame(); 

    m_frameTimer.Begin(m_cmdList.Get());

    CmdBeginEvent(m_cmdList.Get(), "Frame");
    CmdBeginEvent(m_cmdList.Get(), "Clear & Setup");


    //Get buffer to prepare for drawing
    const uint32_t bbIndex = m_swapChain.GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = m_swapChain.GetCurrentBackBuffer();

    //Transition PRESENT -> RENDER_TARGET (manual tracking for now)
    if (m_backBufferStates[bbIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.StateBefore = m_backBufferStates[bbIndex];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; //Draw mode
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(1, &barrier);
        m_backBufferStates[bbIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    //Clear back buffer
    auto rtv = m_swapChain.GetCurrentRTV();
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_cmdList->ClearRenderTargetView(rtv, m_clearColor, 0, nullptr);
    
    CmdBeginEvent(m_cmdList.Get(), "Triangle Pass");
    m_triangle.Render(m_cmdList.Get(), rtv, m_swapChain.Width(), m_swapChain.Height());
    CmdEndEvent(m_cmdList.Get()); //triangle pass

    CmdEndEvent(m_cmdList.Get()); // End Clear & Setup

    CmdBeginEvent(m_cmdList.Get(), "PresentPrep");

    // Transition RENDER_TARGET -> PRESENT
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.StateBefore = m_backBufferStates[bbIndex];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        m_cmdList->ResourceBarrier(1, &barrier);
        m_backBufferStates[bbIndex] = D3D12_RESOURCE_STATE_PRESENT;
    }

    CmdEndEvent(m_cmdList.Get()); // PresentPrep
    CmdEndEvent(m_cmdList.Get()); // Frame

    m_frameTimer.End(m_cmdList.Get());
    m_frameTimer.Resolve(m_cmdList.Get());

    EndFrame();
}

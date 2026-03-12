#include "Application.h"
#include "Source/Renderer/Passes/TrianglePass.h"
#include "Source/Core/Paths.h"
#include "Source/Renderer/Renderer.h"

Renderer m_renderer;

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
    m_frameTimer.Initialize(m_device.GetDevice(), kFrameCount);

    m_timer.Reset();

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

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        // Register the swapchain buffer as starting in PRESENT
        CommandList::SetGlobalState(m_swapChain.GetBuffer(i), D3D12_RESOURCE_STATE_PRESENT);
    }
    
    m_renderer.Initialize(m_device.GetDevice(), m_swapChain.GetFormat(), kFrameCount);

    
    //Called once swapchain has stored initial window size to ensure DepthBuffer matches
    m_renderer.OnResize(m_device.GetDevice(), m_swapChain.Width(), m_swapChain.Height());


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

    const auto shaderDir = Paths::ExecutableDir() / L"Shaders" / L"Compiled";

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
    
    //Depth Buffer
    m_renderer.OnResize(m_device.GetDevice(), w, h);
}

void Application::BeginFrame()
{
    m_frameIndex = m_swapChain.GetCurrentBackBufferIndex();
    FrameContext& frame = m_frames[m_frameIndex];

    //If GPU is still using this frame’s allocator, wait for its fence
    if (frame.fenceValue != 0)
        m_graphicsQueue.Wait(frame.fenceValue);

    //Reset per - frame transient upload allocator, frame is fence safe
    m_renderer.BeginFrame(m_frameIndex);

    //Reset command allocator
    ThrowIfFailed(frame.allocator->Reset(), "Frame Alloc Reset");

    //Open command list, ready to write new commands
    ThrowIfFailed(m_cmdList->Reset(frame.allocator.Get(), nullptr), "CmdList Reset");

    m_cmd.Begin(m_cmdList.Get());

#if defined(_DEBUG)
    const double ms = m_frameTimer.ReadbackMs(m_graphicsQueue.Get(), m_frameIndex);
    if (ms >= 0.0) m_lastGpuMs = ms;

    if ((++m_frameCounter % 60ull) == 0ull && m_lastGpuMs >= 0.0)
    {
        DebugOutput(std::format("GPU frame time: {:.3f} ms", m_lastGpuMs));
    }
#endif
}

void Application::EndFrame()
{
    m_cmd.End();

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close");

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_graphicsQueue.Get()->ExecuteCommandLists(1, lists);
    m_cmd.CommitFinalStates();
    m_swapChain.Present(true);

    // Signal AFTER submit; store per-frame fence value
    const uint64_t fence = m_graphicsQueue.Signal();
    m_frames[m_frameIndex].fenceValue = fence;
}

void Application::Render()
{
    m_timer.Tick();

    BeginFrame(); 

    m_frameTimer.Begin(m_cmdList.Get(), m_frameIndex);

    CmdBeginEvent(m_cmdList.Get(), "Frame");

    //Get buffer to prepare for drawing
    ID3D12Resource* backBuffer = m_swapChain.GetCurrentBackBuffer();
    auto rtv = m_swapChain.GetCurrentRTV();
    
    m_cmd.Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    
    CmdBeginEvent(m_cmdList.Get(), "Render Frame");
    m_renderer.RenderFrame(
        m_device.GetDevice(),
        m_cmd,
        m_frameIndex,
        rtv,
        backBuffer,
        m_swapChain.Width(),
        m_swapChain.Height(),
        m_timer.TotalSeconds()
    );
    CmdEndEvent(m_cmdList.Get()); //Render Frame

    m_cmd.Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT);

    CmdEndEvent(m_cmdList.Get()); // Frame

    m_frameTimer.End(m_cmdList.Get(), m_frameIndex);
    m_frameTimer.Resolve(m_cmdList.Get(), m_frameIndex);

    EndFrame();
}

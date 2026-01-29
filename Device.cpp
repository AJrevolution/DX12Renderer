#include "Device.h"

#if defined(_DEBUG)
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

static bool IsAdapterUsable(IDXGIAdapter1* adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);

    //Skip software adapters (unless WARP is requested separately)
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        return false;

    //Check if it supports D3D12
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
        return false;

    return true;
}

void Device::EnableDebugLayer(bool enable)
{
#if defined(_DEBUG)
    if (!enable) return;

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();

        //Optional GPU-based validation (slower, but catches more)
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1)))
        {
            debug1->SetEnableGPUBasedValidation(TRUE);
        }
    }

    //DXGI debug messages
    ComPtr<IDXGIInfoQueue> infoQueue;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&infoQueue))))
    {
        infoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE);
        // Warning breaks can be noisy, enable if you want:
        // infoQueue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING, TRUE);
    }
#else
    (void)enable;
#endif
}

void Device::Initialize(bool useWarp, bool enableDebugLayer)
{
    EnableDebugLayer(enableDebugLayer);

    UINT factoryFlags = 0;
#if defined(_DEBUG)
    if (enableDebugLayer)
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

    if (useWarp)
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(warp.As(&m_adapter), "warp.As(adapter4)");
    }
    else
    {
        // Pick best hardware adapter by VRAM
        ComPtr<IDXGIAdapter1> best;
        SIZE_T bestVRAM = 0;

        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> a;
            if (m_factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;

            if (!IsAdapterUsable(a.Get()))
                continue;

            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);

            if (desc.DedicatedVideoMemory > bestVRAM)
            {
                best = a;
                bestVRAM = desc.DedicatedVideoMemory;
            }
        }

        if (!best)
            throw std::runtime_error("No suitable DX12 hardware adapter found.");

        ThrowIfFailed(best.As(&m_adapter), "best.As(adapter4)");
    }

    // Create device
    ThrowIfFailed(
        D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)),
        "D3D12CreateDevice"
    );

    // Optional: reduce spam; break on serious messages
#if defined(_DEBUG)
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(m_device.As(&info)))
    {
        info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
#endif

    DXGI_ADAPTER_DESC3 desc{};
    m_adapter->GetDesc3(&desc);

    std::wstring wname(desc.Description);
    DebugOutput(std::format("Using adapter: {}", std::string(wname.begin(), wname.end())));
}

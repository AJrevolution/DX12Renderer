#include "Source/RHI/Diagnostics/GpuTimerSet.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void GPUTimerSet::Initialize(ID3D12Device* device, uint32_t frameCount)
{
#if !defined(_DEBUG)
    (void)device; (void)frameCount;
    return;
#else
    m_frames.resize(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 2;
        ThrowIfFailed(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_frames[i].queryHeap)), "CreateQueryHeap");

        auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * 2);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

        ThrowIfFailed(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &rbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&m_frames[i].readback)),
            "CreateReadbackBuffer"
        );

        SetD3D12ObjectName(m_frames[i].queryHeap.Get(), std::format(L"FrameTimer QueryHeap {}", i).c_str());
        SetD3D12ObjectName(m_frames[i].readback.Get(), std::format(L"FrameTimer Readback {}", i).c_str());
    }
#endif
}

void GPUTimerSet::Begin(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
#if defined(_DEBUG)
    cmd->EndQuery(m_frames[frameIndex].queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
#else
    (void)cmd; (void)frameIndex;
#endif
}

void GPUTimerSet::End(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
#if defined(_DEBUG)
    cmd->EndQuery(m_frames[frameIndex].queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
#else
    (void)cmd; (void)frameIndex;
#endif
}

void GPUTimerSet::Resolve(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex)
{
#if defined(_DEBUG)
    auto& f = m_frames[frameIndex];
    cmd->ResolveQueryData(f.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, f.readback.Get(), 0);
#else
    (void)cmd; (void)frameIndex;
#endif
}

double GPUTimerSet::ReadbackMs(ID3D12CommandQueue* queue, uint32_t frameIndex)
{
#if !defined(_DEBUG)
    (void)queue; (void)frameIndex;
    return -1.0;
#else
    if (m_frequency == 0)
        ThrowIfFailed(queue->GetTimestampFrequency(&m_frequency), "GetTimestampFrequency");

    auto& f = m_frames[frameIndex];

    UINT64* data = nullptr;
    D3D12_RANGE r{ 0, sizeof(UINT64) * 2 };
    if (FAILED(f.readback->Map(0, &r, reinterpret_cast<void**>(&data))) || !data)
        return -1.0;

    const UINT64 begin = data[0];
    const UINT64 end = data[1];

    D3D12_RANGE w{ 0, 0 };
    f.readback->Unmap(0, &w);

    if (end <= begin) return -1.0;

    const double ticks = double(end - begin);
    return (ticks / double(m_frequency)) * 1000.0;
#endif
}

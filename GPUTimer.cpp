#include "GPUTimer.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void GPUTimer::Initialize(ID3D12Device* device)
{
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 2; // begin + end
    ThrowIfFailed(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_queryHeap)), "CreateQueryHeap");

    D3D12_RESOURCE_DESC rb = {};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = sizeof(UINT64) * 2;
    rb.Height = 1;
    rb.DepthOrArraySize = 1;
    rb.MipLevels = 1;
    rb.SampleDesc.Count = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Create a named variable (l-value)
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &rb,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_readback)),
        "CreateReadbackBuffer"
    );

    SetD3D12ObjectName(m_queryHeap.Get(), L"FrameTimer QueryHeap");
    SetD3D12ObjectName(m_readback.Get(), L"FrameTimer Readback");
}

void GPUTimer::Begin(ID3D12GraphicsCommandList* cmd)
{
#if defined(_DEBUG)
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
#else
    (void)cmd;
#endif
}

void GPUTimer::End(ID3D12GraphicsCommandList* cmd)
{
#if defined(_DEBUG)
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
#else
    (void)cmd;
#endif
}

void GPUTimer::Resolve(ID3D12GraphicsCommandList* cmd)
{
#if defined(_DEBUG)
    cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_readback.Get(), 0);
#else
    (void)cmd;
#endif
}

double GPUTimer::ReadbackMs(ID3D12CommandQueue* queue)
{
#if !defined(_DEBUG)
    (void)queue;
    return -1.0;
#else
    if (m_frequency == 0)
        ThrowIfFailed(queue->GetTimestampFrequency(&m_frequency), "GetTimestampFrequency");

    UINT64* data = nullptr;
    D3D12_RANGE r = { 0, sizeof(UINT64) * 2 };
    if (FAILED(m_readback->Map(0, &r, reinterpret_cast<void**>(&data))) || !data)
        return -1.0;

    const UINT64 begin = data[0];
    const UINT64 end = data[1];

    D3D12_RANGE w = { 0, 0 };
    m_readback->Unmap(0, &w);

    if (end <= begin) return -1.0;

    const double ticks = double(end - begin);
    const double ms = (ticks / double(m_frequency)) * 1000.0;
    return ms;
#endif
}

#include "CommandQueue.h"

CommandQueue::~CommandQueue()
{
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void CommandQueue::Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;

    ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");

    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed.");
}

uint64_t CommandQueue::Signal()
{
    const uint64_t value = m_nextFenceValue++;
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), value), "Queue Signal");
    return value;
}

void CommandQueue::Wait(uint64_t fenceValue)
{
    if (m_fence->GetCompletedValue() >= fenceValue)
        return;

    ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent), "SetEventOnCompletion");
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void CommandQueue::Flush()
{
    Wait(Signal());
}

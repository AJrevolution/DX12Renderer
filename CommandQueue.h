#pragma once
#include "Common.h"

class CommandQueue
{
public:
    CommandQueue() = default;
    ~CommandQueue(); // <-- add

    void Initialize(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type);

    ID3D12CommandQueue* Get() const { return m_queue.Get(); }

    uint64_t Signal();
    void Wait(uint64_t fenceValue);
    void Flush();

private:
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence>        m_fence;
    HANDLE                     m_fenceEvent = nullptr;
    uint64_t                   m_nextFenceValue = 1;
};

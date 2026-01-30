#pragma once
#include "Common.h"


class GPUTimer
{
public:
    void Initialize(ID3D12Device* device);
    void Begin(ID3D12GraphicsCommandList* cmd);
    void End(ID3D12GraphicsCommandList* cmd);
    void Resolve(ID3D12GraphicsCommandList* cmd);

    // Call after the GPU finished the frame (i.e., after you waited on the frame fence).
    // Returns milliseconds, or -1 if unavailable.
    double ReadbackMs(ID3D12CommandQueue* queue);

private:
    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource>  m_readback; // 2 timestamps
    UINT64                  m_frequency = 0;
};

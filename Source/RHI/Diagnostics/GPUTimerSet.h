#pragma once
#include "Common.h"
#include <vector>

class GPUTimerSet
{
public:
    void Initialize(ID3D12Device* device, uint32_t frameCount);

    void Begin(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex);
    void End(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex);
    void Resolve(ID3D12GraphicsCommandList* cmd, uint32_t frameIndex);

    // Call after you've waited the frame's fence (BeginFrame) so results are guaranteed ready.
    double ReadbackMs(ID3D12CommandQueue* queue, uint32_t frameIndex);

private:
    struct PerFrame
    {
        ComPtr<ID3D12QueryHeap> queryHeap; // 2 timestamps
        ComPtr<ID3D12Resource>  readback;  // 2x uint64
    };

    std::vector<PerFrame> m_frames;
    UINT64 m_frequency = 0;
};

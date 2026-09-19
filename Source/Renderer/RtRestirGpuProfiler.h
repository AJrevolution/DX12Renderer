#pragma once

#include "Common.h"

#include <array>
#include <cstdint>
#include <vector>

enum class RtRestirGpuTimer : uint32_t
{
    PrimaryDxr = 0,
    Temporal,
    Spatial,
    Resolve,
    Compose,
    Total,
    Count
};

class RtRestirGpuProfiler
{
public:
    static constexpr uint32_t kTimerCount =
        static_cast<uint32_t>(
            RtRestirGpuTimer::Count);

    void Initialize(
        ID3D12Device* device,
        uint32_t frameCount);

    // Call only after the fence for this frame slot has completed.
    void ReadCompletedFrame(
        uint32_t frameIndex,
        uint64_t timestampFrequency);

    void Begin(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        RtRestirGpuTimer timer);

    void End(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        RtRestirGpuTimer timer);

    void Resolve(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex);

    bool HasLastCompletedResult(
        RtRestirGpuTimer timer) const;

    bool HasCompletedResultThisRead(
        RtRestirGpuTimer timer) const;

    double LastCompletedMilliseconds(
        RtRestirGpuTimer timer) const;

private:
    static constexpr uint32_t kQueriesPerTimer = 2u;
    static constexpr uint32_t kQueriesPerFrame =
        kTimerCount * kQueriesPerTimer;

    struct FrameState
    {
        uint32_t begunMask = 0;
        uint32_t endedMask = 0;
        uint32_t resolvedMask = 0;
        bool pendingReadback = false;
    };

    uint32_t QueryIndex(
        uint32_t frameIndex,
        RtRestirGpuTimer timer,
        uint32_t endpoint) const;

private:
    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource> m_readback;

    std::vector<FrameState> m_frames;

    std::array<double, kTimerCount>
        m_lastCompletedMilliseconds{};

    uint32_t m_lastCompletedMask = 0;

    uint32_t m_lastReadMask = 0;
};

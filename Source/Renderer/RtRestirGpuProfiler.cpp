#include "RtRestirGpuProfiler.h"

#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

void RtRestirGpuProfiler::Initialize(
    ID3D12Device* device,
    uint32_t frameCount)
{
    if (!device)
    {
        throw std::runtime_error(
            "RtRestirGpuProfiler::Initialize received null device.");
    }

    if (frameCount == 0)
    {
        throw std::runtime_error(
            "RtRestirGpuProfiler requires at least one frame.");
    }

    m_queryHeap.Reset();
    m_readback.Reset();

    m_frames.clear();
    m_frames.resize(frameCount);

    m_lastCompletedMilliseconds.fill(-1.0);
    m_lastCompletedMask = 0;
    m_lastReadMask = 0;

    const uint64_t totalQueryCount64 =
        static_cast<uint64_t>(frameCount) *
        static_cast<uint64_t>(kQueriesPerFrame);

    if (totalQueryCount64 >
        static_cast<uint64_t>(
            std::numeric_limits<UINT>::max()))
    {
        throw std::runtime_error(
            "RtRestirGpuProfiler query count overflow.");
    }

    const UINT totalQueryCount =
        static_cast<UINT>(
            totalQueryCount64);

    D3D12_QUERY_HEAP_DESC queryHeapDesc{};
    queryHeapDesc.Type =
        D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    queryHeapDesc.Count =
        totalQueryCount;

    ThrowIfFailed(
        device->CreateQueryHeap(
            &queryHeapDesc,
            IID_PPV_ARGS(&m_queryHeap)),
        "Create RT ReSTIR timestamp query heap");

    const uint64_t readbackBytes =
        totalQueryCount64 *
        sizeof(uint64_t);

    const auto readbackDesc =
        CD3DX12_RESOURCE_DESC::Buffer(
            readbackBytes);

    const auto readbackHeap =
        CD3DX12_HEAP_PROPERTIES(
            D3D12_HEAP_TYPE_READBACK);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_readback)),
        "Create RT ReSTIR timestamp readback buffer");

    SetD3D12ObjectName(
        m_queryHeap.Get(),
        L"RT ReSTIR GPU Profiler Query Heap");

    SetD3D12ObjectName(
        m_readback.Get(),
        L"RT ReSTIR GPU Profiler Readback");
}

uint32_t RtRestirGpuProfiler::QueryIndex(
    uint32_t frameIndex,
    RtRestirGpuTimer timer,
    uint32_t endpoint) const
{
    return
        frameIndex *
        kQueriesPerFrame +
        static_cast<uint32_t>(timer) *
        kQueriesPerTimer +
        endpoint;
}

void RtRestirGpuProfiler::Begin(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex,
    RtRestirGpuTimer timer)
{
    if (!commandList ||
        !m_queryHeap ||
        frameIndex >= m_frames.size())
    {
        return;
    }

    const uint32_t timerIndex =
        static_cast<uint32_t>(timer);

    if (timerIndex >= kTimerCount)
        return;

    FrameState& frame =
        m_frames[frameIndex];

    const uint32_t bit =
        1u << timerIndex;

    frame.begunMask |= bit;

    commandList->EndQuery(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        QueryIndex(
            frameIndex,
            timer,
            0u));
}

void RtRestirGpuProfiler::End(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex,
    RtRestirGpuTimer timer)
{
    if (!commandList ||
        !m_queryHeap ||
        frameIndex >= m_frames.size())
    {
        return;
    }

    const uint32_t timerIndex =
        static_cast<uint32_t>(timer);

    if (timerIndex >= kTimerCount)
        return;

    FrameState& frame =
        m_frames[frameIndex];

    const uint32_t bit =
        1u << timerIndex;

    // Do not create an unmatched timestamp pair.
    if ((frame.begunMask & bit) == 0u)
        return;

    commandList->EndQuery(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        QueryIndex(
            frameIndex,
            timer,
            1u));

    frame.endedMask |= bit;
}

void RtRestirGpuProfiler::Resolve(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex)
{
    if (!commandList ||
        !m_queryHeap ||
        !m_readback ||
        frameIndex >= m_frames.size())
    {
        return;
    }

    FrameState& frame =
        m_frames[frameIndex];

    frame.resolvedMask =
        frame.begunMask &
        frame.endedMask;

    if (frame.resolvedMask == 0u)
    {
        frame.pendingReadback = false;
        return;
    }

    for (uint32_t timerIndex = 0;
        timerIndex < kTimerCount;
        ++timerIndex)
    {
        const uint32_t bit =
            1u << timerIndex;

        if ((frame.resolvedMask & bit) == 0u)
            continue;

        const auto timer =
            static_cast<RtRestirGpuTimer>(
                timerIndex);

        const uint32_t queryStart =
            QueryIndex(
                frameIndex,
                timer,
                0u);

        const uint64_t destinationOffset =
            static_cast<uint64_t>(
                queryStart) *
            sizeof(uint64_t);

        commandList->ResolveQueryData(
            m_queryHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            queryStart,
            kQueriesPerTimer,
            m_readback.Get(),
            destinationOffset);
    }

    frame.pendingReadback = true;
}

void RtRestirGpuProfiler::ReadCompletedFrame(
    uint32_t frameIndex,
    uint64_t timestampFrequency)
{
    m_lastReadMask = 0;

    if (frameIndex >= m_frames.size())
        return;

    FrameState& frame =
        m_frames[frameIndex];

    if (!frame.pendingReadback)
    {
        frame = {};
        return;
    }

    if (!m_readback ||
        timestampFrequency == 0)
    {
        frame = {};
        return;
    }

    const uint64_t firstQuery =
        static_cast<uint64_t>(
            frameIndex) *
        kQueriesPerFrame;

    const uint64_t byteBegin =
        firstQuery *
        sizeof(uint64_t);

    const uint64_t byteEnd =
        byteBegin +
        static_cast<uint64_t>(
            kQueriesPerFrame) *
        sizeof(uint64_t);

    D3D12_RANGE readRange{};
    readRange.Begin =
        static_cast<SIZE_T>(
            byteBegin);

    readRange.End =
        static_cast<SIZE_T>(
            byteEnd);

    void* mapped = nullptr;

    const HRESULT mapHr =
        m_readback->Map(
            0,
            &readRange,
            &mapped);

    if (FAILED(mapHr) || !mapped)
    {
        frame = {};
        return;
    }

    const auto* timestamps =
        reinterpret_cast<
        const uint64_t*>(mapped);

    for (uint32_t timerIndex = 0;
        timerIndex < kTimerCount;
        ++timerIndex)
    {
        const uint32_t bit =
            1u << timerIndex;

        if ((frame.resolvedMask & bit) == 0u)
            continue;

        const auto timer =
            static_cast<RtRestirGpuTimer>(
                timerIndex);

        const uint32_t beginQuery =
            QueryIndex(
                frameIndex,
                timer,
                0u);

        const uint32_t endQuery =
            QueryIndex(
                frameIndex,
                timer,
                1u);

        const uint64_t begin =
            timestamps[beginQuery];

        const uint64_t end =
            timestamps[endQuery];

        if (end <= begin)
        {
            m_lastCompletedMilliseconds[
                timerIndex] = -1.0;

            m_lastCompletedMask &=
                ~(1u << timerIndex);

            continue;
        }

        const double deltaTicks =
            static_cast<double>(
                end - begin);

        m_lastCompletedMilliseconds[
            timerIndex] =
            deltaTicks *
                1000.0 /
                static_cast<double>(
                    timestampFrequency);

            m_lastCompletedMask |= bit;
            m_lastReadMask |= bit;
    }

    const D3D12_RANGE writtenRange
    {
        0,
        0
    };

    m_readback->Unmap(
        0,
        &writtenRange);

    frame = {};
}

bool RtRestirGpuProfiler::HasLastCompletedResult(
    RtRestirGpuTimer timer) const
{
    const uint32_t timerIndex =
        static_cast<uint32_t>(timer);

    if (timerIndex >= kTimerCount)
        return false;

    return
        (m_lastCompletedMask &
            (1u << timerIndex)) != 0u;
}

double RtRestirGpuProfiler::LastCompletedMilliseconds(
    RtRestirGpuTimer timer) const
{
    const uint32_t timerIndex =
        static_cast<uint32_t>(timer);

    if (timerIndex >= kTimerCount ||
        !HasLastCompletedResult(timer))
    {
        return -1.0;
    }

    return
        m_lastCompletedMilliseconds[
            timerIndex];
}

bool RtRestirGpuProfiler::HasCompletedResultThisRead(
    RtRestirGpuTimer timer) const
{
    const uint32_t timerIndex =
        static_cast<uint32_t>(timer);

    if (timerIndex >= kTimerCount)
        return false;

    return
        (m_lastReadMask &
            (1u << timerIndex)) != 0u;
}

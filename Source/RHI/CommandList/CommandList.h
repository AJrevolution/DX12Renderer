#pragma once
#include "Common.h"
#include "Source/RHI/CommandList/ResourceStateTracker.h"

class CommandList
{
public:
    CommandList() = default;

    void Begin(ID3D12GraphicsCommandList* cmd);
    void End();

    ID3D12GraphicsCommandList* Get() const { return m_cmd; }

    // State requests
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES newState);
    void UAVBarrier(ID3D12Resource* res);

    void FlushBarriers();

    void CopyBuffer(ID3D12Resource* dst, uint64_t dstOffset, ID3D12Resource* src, uint64_t srcOffset, uint64_t bytes);
    void CopyTexture(const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION& src, const D3D12_BOX* srcBox);

    // Global state helpers for swapchain buffers
    static void SetGlobalState(ID3D12Resource* res, D3D12_RESOURCE_STATES state)
    {
        ResourceStateTracker::SetGlobalState(res, state);
    }

    void CommitFinalStates();
private:
    ID3D12GraphicsCommandList* m_cmd = nullptr;
    ResourceStateTracker m_tracker;
};

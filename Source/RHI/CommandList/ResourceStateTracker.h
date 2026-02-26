#pragma once
#include "Common.h"
#include <unordered_map>
#include <vector>
#include <mutex>

class ResourceStateTracker
{
public:
    void Reset();

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);
    void UAVBarrier(ID3D12Resource* resource);

    // Emit barriers that were generated with known "before" state (local-known transitions).
    void FlushBarriers(ID3D12GraphicsCommandList* cmd);

    // Resolve pending transitions against global states and emit barriers.
    void FlushPendingBarriers(ID3D12GraphicsCommandList* cmd);

    // Update the global state table with the final states for resources used by this command list.
    void CommitFinalStates();

    // Global state helpers (used for swapchain buffers / resources created externally)
    static void SetGlobalState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
    static D3D12_RESOURCE_STATES GetGlobalState(ID3D12Resource* resource);

private:
    struct PendingTransition
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES desired = D3D12_RESOURCE_STATE_COMMON;
    };

private:
    // Per-command-list local tracking
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_localState;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_finalState;

    std::vector<D3D12_RESOURCE_BARRIER> m_barriers;
    std::vector<PendingTransition> m_pending;

    // Global state shared across command lists
    static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> s_globalState;
    static std::mutex s_globalMutex;
};
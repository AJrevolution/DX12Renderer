#include "ResourceStateTracker.h"

std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> ResourceStateTracker::s_globalState;
std::mutex ResourceStateTracker::s_globalMutex;

void ResourceStateTracker::Reset()
{
    m_localState.clear();
    m_finalState.clear();
    m_barriers.clear();
    m_pending.clear();
}

void ResourceStateTracker::SetGlobalState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    std::scoped_lock lock(s_globalMutex);
    s_globalState[resource] = state;
}

D3D12_RESOURCE_STATES ResourceStateTracker::GetGlobalState(ID3D12Resource* resource)
{
    std::scoped_lock lock(s_globalMutex);
    auto it = s_globalState.find(resource);
    if (it == s_globalState.end())
        return D3D12_RESOURCE_STATE_COMMON;
    return it->second;
}

void ResourceStateTracker::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState)
{
    if (!resource) return;

    // If seen in this cmd list, we know the "before"
    auto it = m_localState.find(resource);
    if (it != m_localState.end())
    {
        const D3D12_RESOURCE_STATES before = it->second;
        if (before != newState)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = newState;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_barriers.push_back(b);

            it->second = newState;
        }

        m_finalState[resource] = newState;
        return;
    }

    // First time this cmd list sees this resource -> pending transition resolved later vs global.
    m_localState[resource] = newState;
    m_finalState[resource] = newState;
    m_pending.push_back(PendingTransition{ resource, newState });
}

void ResourceStateTracker::UAVBarrier(ID3D12Resource* resource)
{
    if (!resource) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    m_barriers.push_back(b);
}

void ResourceStateTracker::FlushBarriers(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || m_barriers.empty())
        return;

    cmd->ResourceBarrier((UINT)m_barriers.size(), m_barriers.data());
    m_barriers.clear();
}

void ResourceStateTracker::FlushPendingBarriers(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || m_pending.empty())
        return;

    std::vector<D3D12_RESOURCE_BARRIER> resolved;
    resolved.reserve(m_pending.size());

    {
        std::scoped_lock lock(s_globalMutex);

        for (const auto& p : m_pending)
        {
            const D3D12_RESOURCE_STATES globalBefore =
                (s_globalState.count(p.resource) ? s_globalState[p.resource] : D3D12_RESOURCE_STATE_COMMON);

            if (globalBefore == p.desired)
                continue;

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = p.resource;
            b.Transition.StateBefore = globalBefore;
            b.Transition.StateAfter = p.desired;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            resolved.push_back(b);
        }
    }

    if (!resolved.empty())
        cmd->ResourceBarrier((UINT)resolved.size(), resolved.data());

    m_pending.clear();
}

void ResourceStateTracker::CommitFinalStates()
{
    std::scoped_lock lock(s_globalMutex);
    for (auto& kv : m_finalState)
        s_globalState[kv.first] = kv.second;
}
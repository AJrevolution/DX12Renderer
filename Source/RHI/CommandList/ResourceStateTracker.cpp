#include "ResourceStateTracker.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"
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

    auto it = m_localState.find(resource);

    if (it == m_localState.end())
    {
        m_localState[resource] = std::nullopt;
        m_finalState[resource] = newState;
        m_pending.push_back({ resource, newState });
        return;
    }

    // If we are still in the "Pending" window for this list
    if (!it->second.has_value())
    {
        // Update the existing entry in m_pending so we don't have duplicates
        for (auto& p : m_pending)
        {
            if (p.resource == resource)
            {
                p.desired = newState;
                break;
            }
        }
        m_finalState[resource] = newState;
        return;
    }

    // Normal path: we know the local state, so batch a standard transition
    D3D12_RESOURCE_STATES currentState = it->second.value();
    if (currentState != newState)
    {
        m_barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource, currentState, newState));
        it->second = newState;
    }
    m_finalState[resource] = newState;
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
    if (m_pending.empty()) return;

    std::vector<D3D12_RESOURCE_BARRIER> resolved;
    {
        std::scoped_lock lock(s_globalMutex);
        for (const auto& p : m_pending)
        {
            auto it = s_globalState.find(p.resource);
           
#if defined(_DEBUG) // VALIDATION CODE 
            if (it == s_globalState.end())
            {
                char buf[256];
                sprintf_s(buf, "[RHI Warning]: Resource %p has no global state entry! Defaulting to COMMON. Did you forget to seed it?\n", p.resource);
                OutputDebugStringA(buf);
            }
#endif
            // Look up where it actually is in the world
            D3D12_RESOURCE_STATES globalBefore = (it != s_globalState.end()) ? it->second : D3D12_RESOURCE_STATE_COMMON;
            if (globalBefore != p.desired)
            {
                resolved.push_back(CD3DX12_RESOURCE_BARRIER::Transition(p.resource, globalBefore, p.desired));
            }
            // NOW the local state is truthful
            m_localState[p.resource] = p.desired;
        }
    }

    if (!resolved.empty()) cmd->ResourceBarrier((UINT)resolved.size(), resolved.data());
    m_pending.clear();
}

void ResourceStateTracker::CommitFinalStates()
{
    std::scoped_lock lock(s_globalMutex);
    for (auto& kv : m_finalState)
        s_globalState[kv.first] = kv.second;
}
#include "CommandList.h"

void CommandList::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cmd->ResourceBarrier(1, &b);
}

void CommandList::CopyBuffer(ID3D12Resource* dst, uint64_t dstOffset, ID3D12Resource* src, uint64_t srcOffset, uint64_t bytes)
{
    m_cmd->CopyBufferRegion(dst, dstOffset, src, srcOffset, bytes);
}

#include "CommandList.h"

void CommandList::Begin(ID3D12GraphicsCommandList* cmd)
{
    m_cmd = cmd;
    m_tracker.Reset();
}

void CommandList::End()
{
    
    m_tracker.FlushPendingBarriers(m_cmd);
    m_tracker.FlushBarriers(m_cmd);


}


void CommandList::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES newState)
{
    m_tracker.Transition(res, newState);
}

void CommandList::UAVBarrier(ID3D12Resource* res)
{
    m_tracker.UAVBarrier(res);
}
void CommandList::FlushBarriers()
{

    m_tracker.FlushPendingBarriers(m_cmd);
    m_tracker.FlushBarriers(m_cmd);
}

void CommandList::CopyBuffer(ID3D12Resource* dst, uint64_t dstOffset, ID3D12Resource* src, uint64_t srcOffset, uint64_t bytes)
{
    m_cmd->CopyBufferRegion(dst, dstOffset, src, srcOffset, bytes);
}

void CommandList::CopyTexture(const D3D12_TEXTURE_COPY_LOCATION& dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION& src, const D3D12_BOX* srcBox)
{
    m_cmd->CopyTextureRegion(&dst, x, y, z, &src, srcBox);
}

void CommandList::CommitFinalStates()
{
    m_tracker.CommitFinalStates();
}
#pragma once
#include "Common.h"

class CommandList
{
public:
    explicit CommandList(ID3D12GraphicsCommandList* cmd) : m_cmd(cmd) {}

    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void CopyBuffer(ID3D12Resource* dst, uint64_t dstOffset, ID3D12Resource* src, uint64_t srcOffset, uint64_t bytes);

    ID3D12GraphicsCommandList* Get() const { return m_cmd; }

private:
    ID3D12GraphicsCommandList* m_cmd = nullptr;
};

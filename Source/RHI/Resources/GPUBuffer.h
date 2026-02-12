#pragma once
#include "Common.h"

class GPUBuffer
{
public:
    void CreateDefaultBuffer(ID3D12Device* device, uint64_t sizeBytes, D3D12_RESOURCE_STATES initialState, const wchar_t* name);

    ID3D12Resource* Get() const { return m_resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GPUAddress() const { return m_resource->GetGPUVirtualAddress(); }
    uint64_t SizeBytes() const { return m_sizeBytes; }

private:
    ComPtr<ID3D12Resource> m_resource;
    uint64_t m_sizeBytes = 0;
};

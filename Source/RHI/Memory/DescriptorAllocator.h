#pragma once
#include "Common.h"
#include <mutex>

class DescriptorAllocator
{
public:
    struct Allocation
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; // valid only if heap is shader-visible
        uint32_t index = 0;
        uint32_t count = 1;
    };

    void Initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        uint32_t numDescriptors,
        bool shaderVisible,
        const wchar_t* debugName);

    // Linear allocate count descriptors.
    Allocation Allocate(uint32_t count = 1);

    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
    uint32_t DescriptorSize() const { return m_descriptorSize; }
    bool IsShaderVisible() const { return m_shaderVisible; }

    uint32_t Capacity() const { return m_capacity; }
    uint32_t Used() const { return m_used; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    bool m_shaderVisible = false;

    uint32_t m_capacity = 0;
    uint32_t m_used = 0;
    uint32_t m_descriptorSize = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};

    std::mutex m_mutex;
};

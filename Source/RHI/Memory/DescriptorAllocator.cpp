#include "DescriptorAllocator.h"

void DescriptorAllocator::Initialize(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    uint32_t numDescriptors,
    bool shaderVisible,
    const wchar_t* debugName)
{
    m_type = type;
    m_capacity = numDescriptors;
    m_used = 0;
    m_shaderVisible = shaderVisible;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = numDescriptors;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)), "CreateDescriptorHeap");

    if (debugName)
        SetD3D12ObjectName(m_heap.Get(), debugName);

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart = shaderVisible ? m_heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};
}

DescriptorAllocator::Allocation DescriptorAllocator::Allocate(uint32_t count)
{
    std::scoped_lock lock(m_mutex);

    if (m_used + count > m_capacity)
        throw std::runtime_error("DescriptorAllocator: out of descriptors (increase heap size).");

    Allocation a{};
    a.index = m_used;
    a.count = count;

    a.cpu = m_cpuStart;
    a.cpu.ptr += SIZE_T(a.index) * SIZE_T(m_descriptorSize);

    if (m_shaderVisible)
    {
        a.gpu = m_gpuStart;
        a.gpu.ptr += UINT64(a.index) * UINT64(m_descriptorSize);
    }

    m_used += count;
    return a;
}

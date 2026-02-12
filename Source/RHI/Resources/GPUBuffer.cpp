#include "Source/RHI/Resources/GpuBuffer.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void GPUBuffer::CreateDefaultBuffer(ID3D12Device* device, uint64_t sizeBytes, D3D12_RESOURCE_STATES initialState, const wchar_t* name)
{
    m_sizeBytes = sizeBytes;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&m_resource)),
        "GPUBuffer CreateCommittedResource(DEFAULT)"
    );

    if (name)
        SetD3D12ObjectName(m_resource.Get(), name);
}

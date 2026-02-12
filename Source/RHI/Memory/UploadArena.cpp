#include "Source/RHI/Memory/UploadArena.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

static uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + (a - 1)) & ~(a - 1);
}

void UploadArena::Initialize(ID3D12Device* device, uint32_t frameCount, uint64_t bytesPerFrame)
{
    m_frames.resize(frameCount);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytesPerFrame);

        ThrowIfFailed(
            device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&m_frames[i].buffer)),
            "UploadArena CreateCommittedResource"
        );

        SetD3D12ObjectName(m_frames[i].buffer.Get(), std::format(L"UploadArena Frame {}", i).c_str());

        m_frames[i].capacity = bytesPerFrame;
        m_frames[i].head = 0;

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(m_frames[i].buffer->Map(0, &readRange, &mapped), "UploadArena Map");
        m_frames[i].mapped = reinterpret_cast<uint8_t*>(mapped);
    }
}

void UploadArena::BeginFrame(uint32_t frameIndex)
{
    m_frames[frameIndex].head = 0;
}

UploadArena::Allocation UploadArena::Allocate(uint32_t frameIndex, uint64_t sizeBytes, uint64_t alignment)
{
    auto& f = m_frames[frameIndex];

    const uint64_t aligned = AlignUp(f.head, alignment);
    const uint64_t end = aligned + sizeBytes;

    if (end > f.capacity)
        throw std::runtime_error("UploadArena: out of space (increase bytesPerFrame).");

    Allocation a{};
    a.cpu = f.mapped + aligned;
    a.gpu = f.buffer->GetGPUVirtualAddress() + aligned;
    a.offset = aligned;
    a.size = sizeBytes;

    f.head = end;
    return a;
}

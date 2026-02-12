#pragma once
#include "Common.h"
#include <vector>
#include <cstdint>

class UploadArena
{
public:
    struct Allocation
    {
        uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    void Initialize(ID3D12Device* device, uint32_t frameCount, uint64_t bytesPerFrame);
    void BeginFrame(uint32_t frameIndex); // resets head for that frame

    Allocation Allocate(uint32_t frameIndex, uint64_t sizeBytes, uint64_t alignment = 256);

    // Needed for CopyBufferRegion source
    ID3D12Resource* GetBuffer(uint32_t frameIndex) const { return m_frames[frameIndex].buffer.Get(); }

private:
    struct PerFrame
    {
        ComPtr<ID3D12Resource> buffer; // UPLOAD heap
        uint8_t* mapped = nullptr;     // persistently mapped
        uint64_t capacity = 0;
        uint64_t head = 0;
    };

    std::vector<PerFrame> m_frames;
};

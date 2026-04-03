#pragma once
#include "Common.h"

class AccelerationStructure
{
public:
    struct GeometryDesc
    {
        D3D12_GPU_VIRTUAL_ADDRESS vertexBuffer = 0;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        DXGI_FORMAT vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

        D3D12_GPU_VIRTUAL_ADDRESS indexBuffer = 0;
        uint32_t indexCount = 0;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;

        bool opaque = true;
    };

    struct InstanceDesc
    {
        D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0;
        float transform[12] = {};
        uint32_t instanceID = 0;
        uint32_t hitGroupIndex = 0;
        uint8_t mask = 0xFF;
    };

    void BuildBottomLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        const GeometryDesc& geom,
        const wchar_t* name);

    void BuildTopLevel(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmd,
        const InstanceDesc* instances,
        uint32_t instanceCount,
        const wchar_t* name);

    ID3D12Resource* Resource() const { return m_asBuffer.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const { return m_asBuffer ? m_asBuffer->GetGPUVirtualAddress() : 0; }
    bool IsValid() const { return m_asBuffer != nullptr; }

private:
    void CreateBuffer(
        ID3D12Device* device,
        uint64_t sizeBytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState,
        D3D12_HEAP_TYPE heapType,
        ComPtr<ID3D12Resource>& outBuffer,
        const wchar_t* name);

private:
    ComPtr<ID3D12Resource> m_scratch;
    ComPtr<ID3D12Resource> m_asBuffer;
    ComPtr<ID3D12Resource> m_instanceUpload;
};
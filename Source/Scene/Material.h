#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include <DirectXMath.h>
#include "Source/RHI/Resources/Texture.h"

struct Material
{
    // One contiguous table allocation for this material (space1 t0..tN)
    DescriptorAllocator::Allocation table{}; // gpu points at start of table
   
    // Factors (match shader usage)
    DirectX::XMFLOAT4 baseColorFactor = { 1,1,1,1 };
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.5f;


    // Slot convention within the table (space1):
    // t0 baseColor, t1 normal, t2 metallicRoughness
    static constexpr uint32_t kBaseColorSlot = 0;
    static constexpr uint32_t kNormalSlot = 1;
    static constexpr uint32_t kMetalRoughSlot = 2;
    static constexpr uint32_t kDescriptorCount = 8; //total space reserved

    void UpdateDescriptorTable(ID3D12Device* device, DescriptorAllocator& heap,
        const Texture& albedo, const Texture& normal)
    {
        // Allocate the contiguous block if we haven't already
        if (!table.IsValid())
        {
            table = heap.Allocate(kDescriptorCount);
        }
        const uint32_t descriptorSize = heap.DescriptorSize();
        // Helper to get the specific CPU handle for a slot
        auto GetCpuHandle = [&](uint32_t slot)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = table.cpu;
            h.ptr += (SIZE_T)slot * descriptorSize;
            return h;
        };

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        // Slot 0: Albedo
        srvDesc.Format = albedo.SrvFormat();
        device->CreateShaderResourceView(albedo.Get(), &srvDesc, GetCpuHandle(kBaseColorSlot));
        // Slot 1: Normal
        srvDesc.Format = normal.SrvFormat();
        device->CreateShaderResourceView(normal.Get(), &srvDesc, GetCpuHandle(kNormalSlot));

        // Slot 2: Metallic/Roughness
        /*
        srvDesc.Format = metalRough.SrvFormat();
        device->CreateShaderResourceView(metalRough.Get(), &srvDesc, GetCpuHandle(kMetalRoughSlot));
        */
    }
};

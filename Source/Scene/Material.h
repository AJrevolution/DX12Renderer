#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include <DirectXMath.h>
#include "Source/RHI/Resources/Texture.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"

#include <cstdint>

struct Material
{
    // One contiguous table allocation for this material (space1 t0..tN)
    // Material table space1:
    // t0 baseColor
    // t1 normal
    // t2 metallicRoughness
    // t3 occlusion
    // t4..t7 reserved
    DescriptorAllocator::Allocation table{};    
   
    // Factors (match shader usage)
    DirectX::XMFLOAT4 baseColorFactor = { 1,1,1,1 };
    float metallicFactor = 0.0f;
    float roughnessFactor = 0.5f;

    // glTF occlusion strength defaults to full texture effect.
    // The shader only samples the occlusion texture when hasOcclusionTexture != 0.
    float occlusionStrength = 1.0f;
    uint32_t hasOcclusionTexture = 0;

    // Slot convention within the table (space1):
    // t0 baseColor, t1 normal, t2 metallicRoughness, t3 occlusion
    static constexpr uint32_t kBaseColorSlot = 0;
    static constexpr uint32_t kNormalSlot = 1;
    static constexpr uint32_t kMetalRoughSlot = 2;
    static constexpr uint32_t kDescriptorCount = 8; //total space reserved
    static constexpr uint32_t kOcclusionSlot = 3;

    void UpdateDescriptorTable(
        ID3D12Device* device,
        DescriptorAllocator& heap,
        const Texture* albedo,
        const Texture* normal,
        const Texture* metalRough,
        const Texture* occlusion = nullptr)
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
            h.ptr += static_cast<SIZE_T>(slot) * descriptorSize;            
            return h;
        };

        // Fill all reserved slots with deterministic null SRVs first.
        for (uint32_t slot = 0; slot < kDescriptorCount; ++slot)
        {
            CreateNullTexture2DSRV(
                device,
                GetCpuHandle(slot),
                DXGI_FORMAT_R8G8B8A8_UNORM);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        // Slot 0: Albedo
        if (albedo && albedo->IsValid())
        {
            srvDesc.Format = albedo->SrvFormat();
            device->CreateShaderResourceView(albedo->Get(), &srvDesc, GetCpuHandle(kBaseColorSlot));
        }
        
        // Slot 1: Normal
        if (normal && normal->IsValid())
        {
            srvDesc.Format = normal->SrvFormat();
            device->CreateShaderResourceView(normal->Get(), &srvDesc, GetCpuHandle(kNormalSlot));
        }

        // Slot 2: Metallic/Roughness
        if (metalRough && metalRough->IsValid())
        {
            srvDesc.Format = metalRough->SrvFormat();
            device->CreateShaderResourceView(metalRough->Get(), &srvDesc, GetCpuHandle(kMetalRoughSlot));
        }

        hasOcclusionTexture =
            (occlusion && occlusion->IsValid()) ? 1u : 0u;
        // Slot 3: Occlusion
        if (hasOcclusionTexture != 0u)
        {
            srvDesc.Format = occlusion->SrvFormat();

            device->CreateShaderResourceView(
                occlusion->Get(),
                &srvDesc,
                GetCpuHandle(kOcclusionSlot));
        }
    }
};

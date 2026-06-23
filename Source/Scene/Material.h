#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"
#include <DirectXMath.h>
#include "Source/RHI/Resources/Texture.h"
#include "Source/RHI/Resources/NullSrvHelpers.h"

#include <cstdint>

enum class MaterialAlphaMode : uint32_t
{
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

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

    float normalScale = 1.0f;
    float alphaCutoff = 0.5f;

    DirectX::XMFLOAT4 emissiveFactor = { 0.0f, 0.0f, 0.0f, 0.0f };

    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    uint32_t doubleSided = 0;

    // glTF occlusion strength defaults to full texture effect.
    // The shader only samples the occlusion texture when hasOcclusionTexture != 0.
    float occlusionStrength = 1.0f;
    uint32_t hasOcclusionTexture = 0;
    uint32_t hasEmissiveTexture = 0;
    uint32_t hasNormalTexture = 0;

    uint32_t baseColorTexCoord = 0;
    uint32_t normalTexCoord = 0;
    uint32_t metalRoughTexCoord = 0;
    uint32_t occlusionTexCoord = 0;
    uint32_t emissiveTexCoord = 0;

    const Texture* baseColorTexture = nullptr;
    const Texture* normalTexture = nullptr;
    const Texture* metalRoughTexture = nullptr;
    const Texture* occlusionTexture = nullptr;
    const Texture* emissiveTexture = nullptr;

    // Slot convention within the table (space1):
    // t0 baseColor, t1 normal, t2 metallicRoughness, t3 occlusion, t4 emissive
    static constexpr uint32_t kBaseColorSlot = 0;
    static constexpr uint32_t kNormalSlot = 1;
    static constexpr uint32_t kMetalRoughSlot = 2;
    static constexpr uint32_t kDescriptorCount = 8; //total space reserved
    static constexpr uint32_t kOcclusionSlot = 3;
    static constexpr uint32_t kEmissiveSlot = 4;

    // TODO(Material fallback hardening):
    // Bind semantic raster fallback textures for missing maps:
    // white base color, flat normal, default metallic/roughness, white AO,
    // and black emissive. Current null SRVs are descriptor-safe,
    // but can produce non-ideal material response when assets omit maps.
    void UpdateDescriptorTable(
        ID3D12Device* device,
        DescriptorAllocator& heap,
        const Texture* albedo,
        const Texture* normal,
        const Texture* metalRough,
        const Texture* occlusion = nullptr,
        const Texture* emissive = nullptr)
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

        baseColorTexture =
            (albedo && albedo->IsValid()) ? albedo : nullptr;

        normalTexture =
            (normal && normal->IsValid()) ? normal : nullptr;

        metalRoughTexture =
            (metalRough && metalRough->IsValid()) ? metalRough : nullptr;

        occlusionTexture =
            (occlusion && occlusion->IsValid()) ? occlusion : nullptr;

        emissiveTexture = 
            (emissive && emissive->IsValid()) ? emissive : nullptr;

        hasNormalTexture = normalTexture ? 1u : 0u;
        hasOcclusionTexture = occlusionTexture ? 1u : 0u;
        hasEmissiveTexture = emissiveTexture ? 1u : 0u;


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
        if (emissiveTexture)
        {
            srvDesc.Format = emissiveTexture->SrvFormat();

            device->CreateShaderResourceView(
                emissiveTexture->Get(),
                &srvDesc,
                GetCpuHandle(kEmissiveSlot));
        }

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

        hasOcclusionTexture = occlusionTexture ? 1u : 0u;
        // Slot 3: Occlusion
        if (hasOcclusionTexture)
        {
            srvDesc.Format = occlusionTexture->SrvFormat();

            device->CreateShaderResourceView(
                occlusionTexture->Get(),
                &srvDesc,
                GetCpuHandle(kOcclusionSlot));
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvTable() const
    {
        return table.IsValid()
            ? table.gpu
            : D3D12_GPU_DESCRIPTOR_HANDLE{};
    }

    bool HasSrvTable() const
    {
        return table.IsValid();
    }
};

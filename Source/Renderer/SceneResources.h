#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"

struct SceneResources
{
    enum Slot : uint32_t
    {
        BRDF_LUT = 0,
        IBL_DIFFUSE,
        IBL_SPECULAR,
        SHADOW_MAP,
        COUNT
    };

    DescriptorAllocator::Allocation table{};

    bool IsValid() const { return table.IsValid(); }
};

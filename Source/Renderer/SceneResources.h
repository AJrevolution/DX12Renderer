#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"

// Scene-wide descriptor table bound at root param 2 (space0).
// Slot contract is fixed so future IBL/shadow hookups do not require root signature changes.
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

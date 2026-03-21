#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"

// Scene-wide descriptor table bound at root param 2 (space0).
// Current Phase 5.6 bindings:
//  - BRDF LUT is a real 2D texture
//  - IBL diffuse/specular are temporary latlong 2D env maps
//  - shadow slot is reserved
// The slot contract is fixed so later cubemap/prefilter upgrades do not require root signature changes.
struct SceneResources
{
    enum Slot : uint32_t
    {
        BRDF_LUT = 0,   // t0, space0
        IBL_DIFFUSE = 1,// t1, space0
        IBL_SPECULAR = 2,// t2, space0
        SHADOW_MAP = 3, // t3, space0
        COUNT
    };

    DescriptorAllocator::Allocation table{};

    bool IsValid() const { return table.IsValid(); }
};

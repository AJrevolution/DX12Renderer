#pragma once
#include "Source/RHI/Memory/DescriptorAllocator.h"

// Minimal: one base color texture SRV allocation.
// Later add normal/metal-rough/AO/etc.
struct Material
{
    DescriptorAllocator::Allocation baseColorSrv{};
};

#pragma once
#include "Common.h"
#include <filesystem>

namespace Paths
{
    //Directory containing the executable.
    std::filesystem::path ExecutableDir();

    // Runtime shader directory 
    // DXC output set to $(OutDir)\Shaders\Compiled, this resolves to:
    std::filesystem::path ShaderDir();

    // Returns empty path if not found. Debug
    std::filesystem::path ContentDir_DevOnly();
}
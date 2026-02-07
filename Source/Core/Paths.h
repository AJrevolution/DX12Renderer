#pragma once
#include "Common.h"
#include <filesystem>

inline std::filesystem::path GetExecutableDir()
{
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return p.parent_path();
}

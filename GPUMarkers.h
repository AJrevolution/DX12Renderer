#pragma once
#include "Common.h"
#include <string_view>
#if defined(_DEBUG)
#include <pix3.h> // from WinPixEventRuntime NuGet
#endif

inline void CmdBeginEvent(ID3D12GraphicsCommandList* cmd, const char* name)
{
#if defined(_DEBUG)
    if (!cmd || !name) return;
    PIXBeginEvent(cmd, PIX_COLOR_DEFAULT, "%s", name);
#else
    (void)cmd; (void)name;
#endif
}

inline void CmdEndEvent(ID3D12GraphicsCommandList* cmd)
{
#if defined(_DEBUG)
    if (!cmd) return;
    PIXEndEvent(cmd);
#else
    (void)cmd;
#endif
}

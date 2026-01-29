#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <wrl/client.h>
#include <string>
#include <stdexcept>
#include <format>

#include <d3d12.h>
#include <dxgi1_6.h>

using Microsoft::WRL::ComPtr;

inline void ThrowIfFailed(HRESULT hr, const char* msg = "")
{
    if (FAILED(hr))
    {
        std::string what = std::format("HRESULT 0x{:08X} {}", (unsigned)hr, msg);
        throw std::runtime_error(what);
    }
}

inline void DebugOutput(const std::string& s)
{
    OutputDebugStringA((s + "\n").c_str());
}

inline void SetD3D12ObjectName(ID3D12Object* obj, const wchar_t* name)
{
#if defined(_DEBUG)
    if (obj && name) obj->SetName(name);
#else
    (void)obj; (void)name;
#endif
}

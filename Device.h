#pragma once
#include "Common.h"

class Device
{
public:
    void Initialize(bool useWarp = false, bool enableDebugLayer = true);

    ID3D12Device* GetDevice()  const { return m_device.Get(); }
    IDXGIFactory7* GetFactory() const { return m_factory.Get(); }
    IDXGIAdapter4* GetAdapter() const { return m_adapter.Get(); }

private:
    void EnableDebugLayer(bool enable);

private:
    ComPtr<IDXGIFactory7> m_factory;
    ComPtr<IDXGIAdapter4> m_adapter;
    ComPtr<ID3D12Device>  m_device;
};

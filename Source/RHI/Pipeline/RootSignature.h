#pragma once
#include "Common.h"

class RootSignature
{
public:
    void InitializeEmpty(ID3D12Device* device);

    ID3D12RootSignature* Get() const { return m_rootSig.Get(); }

private:
    ComPtr<ID3D12RootSignature> m_rootSig;
};

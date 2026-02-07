#pragma once
#include "Common.h"

class PipelineState
{
public:
    void InitialiseTriangle(
        ID3D12Device* device,
        ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps,
        DXGI_FORMAT rtvFormat
    );

    ID3D12PipelineState* Get() const { return m_pso.Get(); }

private:
    ComPtr<ID3D12PipelineState> m_pso;
};

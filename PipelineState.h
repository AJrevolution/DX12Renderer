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

    void InitialiseForwardPBR(
        ID3D12Device* device,
        ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps,
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT dsvFormat,
        D3D12_CULL_MODE cullMode,
        bool frontCounterClockwise = false
    );

    void InitialiseGBuffer(
        ID3D12Device* device,
        ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps,
        DXGI_FORMAT rt0,
        DXGI_FORMAT rt1,
        DXGI_FORMAT rt2,
        DXGI_FORMAT rt3,
        DXGI_FORMAT dsvFormat,
        D3D12_CULL_MODE cullMode,
        bool frontCounterClockwise = false
    );

    void InitialiseDeferredLight(
        ID3D12Device* device,
        ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps,
        DXGI_FORMAT rtvFormat
    );

    void InitialiseShadow(
        ID3D12Device* device,
        ID3D12RootSignature* rootSig,
        D3D12_SHADER_BYTECODE vs,
        D3D12_SHADER_BYTECODE ps,
        DXGI_FORMAT dsvFormat,
        D3D12_CULL_MODE cullMode,
        bool frontCounterClockwise = false);

    ID3D12PipelineState* Get() const { return m_pso.Get(); }

private:
    ComPtr<ID3D12PipelineState> m_pso;
};

#pragma once
#include <filesystem>
#include <fstream>
#include "Common.h"
#include "Source/RHI/CommandList/CommandList.h"
#include "ThirdParty\DirectX-Headers\include\directx\d3dx12.h"
#include <DirectXMath.h>

class RtDenoisePass
{
public:
    void Initialize(ID3D12Device* device, const std::filesystem::path& shaderDir);

    void Dispatch(
        CommandList& cl,
        D3D12_GPU_DESCRIPTOR_HANDLE inputSrvTable,
        D3D12_GPU_DESCRIPTOR_HANDLE outputUav,
        uint32_t width,
        uint32_t height,
        int radius,
        float sigmaDepth,
        float sigmaNormal);

private:
    struct Constants
    {
        DirectX::XMFLOAT2 invResolution = {};
        int radius = 2;
        float sigmaDepth = 0.02f;
        float sigmaNormal = 0.25f;
        float normalPower = 64.0f;
        float pad[2] = {};
    };

    void BuildRootSignature(ID3D12Device* device);
    void BuildPipelineState(ID3D12Device* device, const std::filesystem::path& shaderPath);

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
};
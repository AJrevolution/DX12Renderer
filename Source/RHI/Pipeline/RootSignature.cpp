#include "Source/RHI/Pipeline/RootSignature.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void RootSignature::InitializeEmpty(ID3D12Device* device)
{
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
#if defined(_DEBUG)
    if (FAILED(hr) && error)
        OutputDebugStringA((const char*)error->GetBufferPointer());
#endif
    ThrowIfFailed(hr, "D3D12SerializeRootSignature");

    ThrowIfFailed(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)),
        "CreateRootSignature"
    );

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: Triangle (Empty)");
}

void RootSignature::InitializeMain(ID3D12Device* device)
{
    // Define the SRV range (Scalable Intent)
    // reserve 8 slots (t0 through t7) in space 1 for textures.
    static constexpr UINT kMaterialSrvCount = 8;
    CD3DX12_DESCRIPTOR_RANGE texRange{};
    texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaterialSrvCount, 0, 1); // t0..t7, space 1

    // Define Root Parameters
    // Index 0: Per-frame Constant Buffer (Root CBV)
    // Index 1: Per-object Constant Buffer (Root CBV)
    // Index 2: Material Textures (Descriptor Table)
    CD3DX12_ROOT_PARAMETER params[3]{};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);   // b0, space 0
    params[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);   // b1, space 0
    params[2].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);

    // Define a Static Sampler (for later commit)
    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0, // shader register s0
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP
    );

    // Serialize and Create
    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr) && error)
    {
        OutputDebugStringA((const char*)error->GetBufferPointer());
    }
    ThrowIfFailed(hr, "D3D12SerializeRootSignature (Main)");

    ThrowIfFailed(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)),
        "CreateRootSignature (Main)"
    );

    SetD3D12ObjectName(m_rootSig.Get(), L"RootSig: Main");
}
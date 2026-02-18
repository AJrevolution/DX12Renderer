#include "Source/RHI/Pipeline/RootSignature.h"

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

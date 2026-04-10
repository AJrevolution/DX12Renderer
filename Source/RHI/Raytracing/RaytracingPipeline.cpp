#include "RaytracingPipeline.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"
#include <fstream>

static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open RT shader library.");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void RaytracingPipeline::Initialize(ID3D12Device5* device, const std::filesystem::path& shaderDir)
{
    BuildRootSignature(device);
    BuildStateObject(device, shaderDir / L"Raytracing.cso");
    BuildShaderTable(device);
}

void RaytracingPipeline::BuildRootSignature(ID3D12Device* device)
{
    CD3DX12_DESCRIPTOR_RANGE uavTable;
    uavTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0); // u0 output texture

    CD3DX12_DESCRIPTOR_RANGE srvTable;
    srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 1, 0); // t1..t5 geometry buffers

    CD3DX12_ROOT_PARAMETER params[4]{};
    params[0].InitAsDescriptorTable(1, &uavTable); // u0 output texture
    params[1].InitAsShaderResourceView(0);         // t0 TLAS
    params[2].InitAsConstantBufferView(0);         // b0 frame
    params[3].InitAsDescriptorTable(1, &srvTable); // t1..t5 geometry buffers

    CD3DX12_ROOT_SIGNATURE_DESC desc{};
    desc.Init(_countof(params), params, 0, nullptr);

    ComPtr<ID3DBlob> blob, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), "Serialize RT root sig");
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_globalRootSig)), "Create RT root sig");
    SetD3D12ObjectName(m_globalRootSig.Get(), L"RootSig: DXR Global");
}

void RaytracingPipeline::BuildStateObject(ID3D12Device5* device, const std::filesystem::path& shaderPath)
{
    const auto bytes = ReadFileBytes(shaderPath);

    D3D12_EXPORT_DESC exports[] =
    {
        { L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"Miss", nullptr, D3D12_EXPORT_FLAG_NONE },
        { L"ClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE },
    };

    D3D12_DXIL_LIBRARY_DESC dxil{};
    D3D12_SHADER_BYTECODE lib{};
    lib.pShaderBytecode = bytes.data();
    lib.BytecodeLength = bytes.size();
    dxil.DXILLibrary = lib;
    dxil.NumExports = _countof(exports);
    dxil.pExports = exports;

    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport = L"HitGroup";
    hitGroup.ClosestHitShaderImport = L"ClosestHit";
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = 24;
    shaderConfig.MaxAttributeSizeInBytes = 8;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = m_globalRootSig.Get();

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = 2;

    D3D12_STATE_SUBOBJECT subobjects[5]{};
    subobjects[0] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil };
    subobjects[1] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup };
    subobjects[2] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig };
    subobjects[3] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };
    subobjects[4] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig };

    D3D12_STATE_OBJECT_DESC desc{};
    desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    desc.NumSubobjects = _countof(subobjects);
    desc.pSubobjects = subobjects;

    ThrowIfFailed(device->CreateStateObject(&desc, IID_PPV_ARGS(&m_stateObject)), "Create RT state object");
    ThrowIfFailed(m_stateObject->QueryInterface(IID_PPV_ARGS(&m_stateObjectProps)), "QI RT state object props");
}

void RaytracingPipeline::BuildShaderTable(ID3D12Device5* device)
{
    const uint32_t idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    const uint32_t recordAlign = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32
    const uint32_t tableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64

    auto Align = [](uint32_t v, uint32_t a)
    {
        return (v + a - 1) & ~(a - 1);
    };

    m_rayGenRecordSize = Align(idSize, recordAlign);
    m_missRecordSize = Align(idSize, recordAlign);
    m_hitRecordSize = Align(idSize, recordAlign);

    m_rayGenOffset = 0;
    m_missOffset = Align(m_rayGenOffset + m_rayGenRecordSize, tableAlign);
    m_hitOffset = Align(m_missOffset + m_missRecordSize, tableAlign);

    const uint32_t totalSize = Align(m_hitOffset + m_hitRecordSize, tableAlign);


    CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_shaderTable)),
        "Create shader table");

    SetD3D12ObjectName(m_shaderTable.Get(), L"DXR ShaderTable");

    uint8_t* mapped = nullptr;
    D3D12_RANGE range{ 0, 0 };
    ThrowIfFailed(
        m_shaderTable->Map(0, &range, reinterpret_cast<void**>(&mapped)),
        "Map shader table");

    memcpy(mapped + m_rayGenOffset, m_stateObjectProps->GetShaderIdentifier(L"RayGen"), idSize);
    memcpy(mapped + m_missOffset, m_stateObjectProps->GetShaderIdentifier(L"Miss"), idSize);
    memcpy(mapped + m_hitOffset, m_stateObjectProps->GetShaderIdentifier(L"HitGroup"), idSize);

    m_shaderTable->Unmap(0, nullptr);
}
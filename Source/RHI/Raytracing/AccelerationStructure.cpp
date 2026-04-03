#include "Source/RHI/Raytracing/AccelerationStructure.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

void AccelerationStructure::CreateBuffer(
    ID3D12Device* device,
    uint64_t sizeBytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState,
    D3D12_HEAP_TYPE heapType,
    ComPtr<ID3D12Resource>& outBuffer,
    const wchar_t* name)
{
    CD3DX12_HEAP_PROPERTIES heap(heapType);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes, flags);

    ThrowIfFailed(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outBuffer)),
        "CreateCommittedResource(AS buffer)");

    if (name)
        SetD3D12ObjectName(outBuffer.Get(), name);
}

void AccelerationStructure::BuildBottomLevel(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* cmd,
    const GeometryDesc& geom,
    const wchar_t* name)
{
    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = geom.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
        : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

    g.Triangles.Transform3x4 = 0;
    g.Triangles.IndexFormat = geom.indexFormat;
    g.Triangles.VertexFormat = geom.vertexFormat;
    g.Triangles.IndexCount = geom.indexCount;
    g.Triangles.VertexCount = geom.vertexCount;
    g.Triangles.IndexBuffer = geom.indexBuffer;
    g.Triangles.VertexBuffer.StartAddress = geom.vertexBuffer;
    g.Triangles.VertexBuffer.StrideInBytes = geom.vertexStride;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &g;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    CreateBuffer(
        device,
        info.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_HEAP_TYPE_DEFAULT,
        m_asBuffer,
        name);

    CreateBuffer(
        device,
        info.ScratchDataSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_HEAP_TYPE_DEFAULT,
        m_scratch,
        L"BLAS Scratch");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData = m_asBuffer->GetGPUVirtualAddress();

    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_asBuffer.Get();
    cmd->ResourceBarrier(1, &uav);
}

void AccelerationStructure::BuildTopLevel(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* cmd,
    const InstanceDesc* instances,
    uint32_t instanceCount,
    const wchar_t* name)
{
    const uint64_t uploadBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount;

    CreateBuffer(
        device,
        uploadBytes,
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_HEAP_TYPE_UPLOAD,
        m_instanceUpload,
        L"TLAS Instance Upload");

    auto* mapped = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(nullptr);
    D3D12_RANGE range{ 0, 0 };
    ThrowIfFailed(m_instanceUpload->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map(TLAS instances)");

    for (uint32_t i = 0; i < instanceCount; ++i)
    {
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        memcpy(d.Transform, instances[i].transform, sizeof(float) * 12);
        d.InstanceID = instances[i].instanceID;
        d.InstanceContributionToHitGroupIndex = instances[i].hitGroupIndex;
        d.InstanceMask = instances[i].mask;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        d.AccelerationStructure = instances[i].blasAddress;
        mapped[i] = d;
    }

    m_instanceUpload->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = instanceCount;
    inputs.InstanceDescs = m_instanceUpload->GetGPUVirtualAddress();
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

    CreateBuffer(
        device,
        info.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_HEAP_TYPE_DEFAULT,
        m_asBuffer,
        name);

    CreateBuffer(
        device,
        info.ScratchDataSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_HEAP_TYPE_DEFAULT,
        m_scratch,
        L"TLAS Scratch");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
    build.DestAccelerationStructureData = m_asBuffer->GetGPUVirtualAddress();

    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = m_asBuffer.Get();
    cmd->ResourceBarrier(1, &uav);
}
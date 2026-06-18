#include "Mesh.h"
#include "ThirdParty/DirectX-Headers/include/directx/d3dx12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    Float3 operator+(Float3 a, Float3 b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    Float3 operator-(Float3 a, Float3 b)
    {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    Float3 operator*(Float3 a, float s)
    {
        return { a.x * s, a.y * s, a.z * s };
    }

    Float3& operator+=(Float3& a, Float3 b)
    {
        a.x += b.x;
        a.y += b.y;
        a.z += b.z;
        return a;
    }

    float Dot(Float3 a, Float3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Float3 Cross(Float3 a, Float3 b)
    {
        return
        {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    float LengthSq(Float3 v)
    {
        return Dot(v, v);
    }

    Float3 NormalizeOr(Float3 v, Float3 fallback)
    {
        const float lenSq = LengthSq(v);

        if (lenSq <= 1e-12f)
            return fallback;

        const float invLen = 1.0f / std::sqrt(lenSq);
        return v * invLen;
    }

    Float3 PositionOf(const Mesh::Vertex& v)
    {
        return { v.px, v.py, v.pz };
    }

    Float3 NormalOf(const Mesh::Vertex& v)
    {
        return NormalizeOr({ v.nx, v.ny, v.nz }, { 0.0f, 1.0f, 0.0f });
    }

    Float3 FallbackTangentForNormal(Float3 n)
    {
        const Float3 up =
            (std::fabs(n.y) < 0.999f)
            ? Float3{ 0.0f, 1.0f, 0.0f }
        : Float3{ 1.0f, 0.0f, 0.0f };

        return NormalizeOr(Cross(up, n), { 1.0f, 0.0f, 0.0f });
    }

    std::vector<Mesh::Submesh> ValidateSubmeshes(
        const std::vector<Mesh::Submesh>& input,
        uint32_t indexCount,
        const Mesh::Submesh& fallback)
    {
        std::vector<Mesh::Submesh> output;
        output.reserve(input.size());

        for (const Mesh::Submesh& submesh : input)
        {
            if (submesh.indexCount == 0)
                continue;

            if ((submesh.indexStart % 3) != 0 ||
                (submesh.indexCount % 3) != 0)
            {
                continue;
            }

            if (submesh.indexStart >= indexCount)
                continue;

            if (submesh.indexCount > indexCount - submesh.indexStart)
                continue;

            output.push_back(submesh);
        }

        if (output.empty())
        {
            output.push_back(fallback);
        }

        return output;
    }

    bool IndicesAreValid(
        const std::vector<uint32_t>& indices,
        size_t vertexCount)
    {
        if (indices.empty() || (indices.size() % 3) != 0)
            return false;

        for (uint32_t index : indices)
        {
            if (index >= vertexCount)
                return false;
        }

        return true;
    }
}

void Mesh::GenerateTangents(
    std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.size() < 3)
        return;

    std::vector<Float3> tangentAccum(vertices.size());
    std::vector<Float3> bitangentAccum(vertices.size());

    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];

        if (i0 >= vertices.size() ||
            i1 >= vertices.size() ||
            i2 >= vertices.size())
        {
            continue;
        }

        const Vertex& v0 = vertices[i0];
        const Vertex& v1 = vertices[i1];
        const Vertex& v2 = vertices[i2];

        const Float3 p0 = PositionOf(v0);
        const Float3 p1 = PositionOf(v1);
        const Float3 p2 = PositionOf(v2);

        const Float3 e1 = p1 - p0;
        const Float3 e2 = p2 - p0;

        const float du1 = v1.u - v0.u;
        const float dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u;
        const float dv2 = v2.v - v0.v;

        const float det = du1 * dv2 - du2 * dv1;

        if (std::fabs(det) <= 1e-8f)
            continue;

        const float invDet = 1.0f / det;

        const Float3 tangent =
        {
            (e1.x * dv2 - e2.x * dv1) * invDet,
            (e1.y * dv2 - e2.y * dv1) * invDet,
            (e1.z * dv2 - e2.z * dv1) * invDet
        };

        const Float3 bitangent =
        {
            (e2.x * du1 - e1.x * du2) * invDet,
            (e2.y * du1 - e1.y * du2) * invDet,
            (e2.z * du1 - e1.z * du2) * invDet
        };

        tangentAccum[i0] += tangent;
        tangentAccum[i1] += tangent;
        tangentAccum[i2] += tangent;

        bitangentAccum[i0] += bitangent;
        bitangentAccum[i1] += bitangent;
        bitangentAccum[i2] += bitangent;
    }

    for (size_t i = 0; i < vertices.size(); ++i)
    {
        Vertex& v = vertices[i];

        const Float3 n = NormalOf(v);
        Float3 t = tangentAccum[i];

        // Gram-Schmidt orthogonalisation.
        t = t - n * Dot(n, t);
        t = NormalizeOr(t, FallbackTangentForNormal(n));

        const Float3 b = bitangentAccum[i];
        const float handedness =
            Dot(Cross(n, t), b) < 0.0f ? -1.0f : 1.0f;

        v.tx = t.x;
        v.ty = t.y;
        v.tz = t.z;
        v.tw = handedness;
    }
}

void Mesh::SetSingleWholeMeshSubmesh()
{
    m_wholeMeshSubmesh = {};
    m_wholeMeshSubmesh.indexStart = 0;
    m_wholeMeshSubmesh.indexCount = m_indexCount;
    m_wholeMeshSubmesh.vertexBase = 0;
    m_wholeMeshSubmesh.materialIndex = 0;

    m_submeshes.clear();

    if (m_indexCount > 0)
    {
        m_submeshes.push_back(m_wholeMeshSubmesh);
    }
}

void Mesh::CreateFromData(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex,
    const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<Submesh>& submeshes,
    const wchar_t* debugName,
    bool generateTangents)
{
    m_vertexCount = 0;
    m_indexCount = 0;
    m_vbv = {};
    m_ibv = {};
    m_wholeMeshSubmesh = {};
    m_submeshes.clear();

    if (!device ||
        vertices.empty() ||
        indices.empty() ||
        !IndicesAreValid(indices, vertices.size()))
        return;

    std::vector<Vertex> vertexData = vertices;

    if (generateTangents)
    {
        GenerateTangents(vertexData, indices);
    }

    if (vertexData.size() > std::numeric_limits<uint32_t>::max() / sizeof(Vertex) ||
        indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t))
    {
        return;
    }

    const uint32_t vbSize =
        static_cast<uint32_t>(vertexData.size() * sizeof(Vertex));

    const uint32_t ibSize =
        static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

    const wchar_t* safeDebugName =
        debugName ? debugName : L"Mesh";

    const std::wstring vbName =
        std::wstring(L"VB: ") + safeDebugName;

    const std::wstring ibName =
        std::wstring(L"IB: ") + safeDebugName;

    m_vb.CreateDefaultBuffer(
        device,
        vbSize,
        D3D12_RESOURCE_STATE_COPY_DEST,
        vbName.c_str());

    m_ib.CreateDefaultBuffer(
        device,
        ibSize,
        D3D12_RESOURCE_STATE_COPY_DEST,
        ibName.c_str());

    CommandList::SetGlobalState(
        m_vb.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST);

    CommandList::SetGlobalState(
        m_ib.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST);

    auto vbAlloc = upload.Allocate(frameIndex, vbSize, 16);
    std::memcpy(vbAlloc.cpu, vertexData.data(), vbSize);

    cl.CopyBuffer(
        m_vb.Get(),
        0,
        upload.GetBuffer(frameIndex),
        vbAlloc.offset,
        vbSize);

    cl.Transition(
        m_vb.Get(),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    auto ibAlloc = upload.Allocate(frameIndex, ibSize, 16);
    std::memcpy(ibAlloc.cpu, indices.data(), ibSize);

    cl.CopyBuffer(
        m_ib.Get(),
        0,
        upload.GetBuffer(frameIndex),
        ibAlloc.offset,
        ibSize);

    cl.Transition(
        m_ib.Get(),
        D3D12_RESOURCE_STATE_INDEX_BUFFER);

    m_vertexCount = static_cast<uint32_t>(vertexData.size());
    m_indexCount = static_cast<uint32_t>(indices.size());

    m_vbv.BufferLocation = m_vb.GPUAddress();
    m_vbv.SizeInBytes = vbSize;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib.GPUAddress();
    m_ibv.SizeInBytes = ibSize;
    m_ibv.Format = DXGI_FORMAT_R32_UINT;

    m_wholeMeshSubmesh = {};
    m_wholeMeshSubmesh.indexStart = 0;
    m_wholeMeshSubmesh.indexCount = m_indexCount;
    m_wholeMeshSubmesh.vertexBase = 0;
    m_wholeMeshSubmesh.materialIndex = 0;

    m_submeshes =
        ValidateSubmeshes(
            submeshes,
            m_indexCount,
            m_wholeMeshSubmesh);
}


void Mesh::CreateTexturedQuad(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex)
{
    const Vertex verts[] =
    {
        // Position            Normal         Tangent(xyz,w)      Color         UV
        { -0.5f,  0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      0,0 }, // 0: Top-Left
        {  0.5f,  0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      1,0 }, // 1: Top-Right
        {  0.5f, -0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      1,1 }, // 2: Bottom-Right
        { -0.5f, -0.5f, 0.0f,  0,0,-1,       1,0,0, 1,           1,1,1,1,      0,1 }, // 3: Bottom-Left
    };

    const uint16_t indices[] =
    {
        0, 1, 2,
        0, 2, 3
    };

    const uint32_t vbSize = sizeof(verts);
    const uint32_t ibSize = sizeof(indices);
    m_indexCount = 6;
    m_vertexCount = 4;

    // Create DEFAULT VB/IB in COPY_DEST
    m_vb.CreateDefaultBuffer(device, vbSize, D3D12_RESOURCE_STATE_COPY_DEST, L"VB: Quad (DEFAULT)");
    m_ib.CreateDefaultBuffer(device, ibSize, D3D12_RESOURCE_STATE_COPY_DEST, L"IB: Quad (DEFAULT)");

    CommandList::SetGlobalState(m_vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    CommandList::SetGlobalState(m_ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

    // Stage vertex data
    auto vbAlloc = upload.Allocate(frameIndex, vbSize, 16);
    memcpy(vbAlloc.cpu, verts, vbSize);

    cl.CopyBuffer(m_vb.Get(), 0, upload.GetBuffer(frameIndex), vbAlloc.offset, vbSize);

    cl.Transition(m_vb.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Stage index data
    auto ibAlloc = upload.Allocate(frameIndex, ibSize, 16);
    memcpy(ibAlloc.cpu, indices, ibSize);

    cl.CopyBuffer(m_ib.Get(), 0, upload.GetBuffer(frameIndex), ibAlloc.offset, ibSize);

    cl.Transition(m_ib.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Views
    m_vbv.BufferLocation = m_vb.GPUAddress();
    m_vbv.SizeInBytes = vbSize;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib.GPUAddress();
    m_ibv.SizeInBytes = ibSize;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
    
    SetSingleWholeMeshSubmesh();
}

void Mesh::CreateFloorPlane(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    uint32_t frameIndex)
{
    const Vertex verts[] =
    {
        // Position               Normal        Tangent(xyz,w)     Color         UV
        { -3.0f, 0.0f,  3.0f,     0,1,0,       1,0,0,1,           1,1,1,1,      0,0 },
        {  3.0f, 0.0f,  3.0f,     0,1,0,       1,0,0,1,           1,1,1,1,      1,0 },
        {  3.0f, 0.0f, -3.0f,     0,1,0,       1,0,0,1,           1,1,1,1,      1,1 },
        { -3.0f, 0.0f, -3.0f,     0,1,0,       1,0,0,1,           1,1,1,1,      0,1 },
    };

    const uint16_t indices[] =
    {
        0, 1, 2,
        0, 2, 3
    };

    const uint32_t vbSize = sizeof(verts);
    const uint32_t ibSize = sizeof(indices);
    m_indexCount = 6;
    m_vertexCount = 4;

    m_vb.CreateDefaultBuffer(device, vbSize, D3D12_RESOURCE_STATE_COPY_DEST, L"VB: Floor (DEFAULT)");
    m_ib.CreateDefaultBuffer(device, ibSize, D3D12_RESOURCE_STATE_COPY_DEST, L"IB: Floor (DEFAULT)");

    CommandList::SetGlobalState(m_vb.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    CommandList::SetGlobalState(m_ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

    auto vbAlloc = upload.Allocate(frameIndex, vbSize, 16);
    memcpy(vbAlloc.cpu, verts, vbSize);
    cl.CopyBuffer(m_vb.Get(), 0, upload.GetBuffer(frameIndex), vbAlloc.offset, vbSize);
    cl.Transition(m_vb.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    auto ibAlloc = upload.Allocate(frameIndex, ibSize, 16);
    memcpy(ibAlloc.cpu, indices, ibSize);
    cl.CopyBuffer(m_ib.Get(), 0, upload.GetBuffer(frameIndex), ibAlloc.offset, ibSize);
    cl.Transition(m_ib.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER);

    m_vbv.BufferLocation = m_vb.GPUAddress();
    m_vbv.SizeInBytes = vbSize;
    m_vbv.StrideInBytes = sizeof(Vertex);

    m_ibv.BufferLocation = m_ib.GPUAddress();
    m_ibv.SizeInBytes = ibSize;
    m_ibv.Format = DXGI_FORMAT_R16_UINT;
    
    SetSingleWholeMeshSubmesh();
}

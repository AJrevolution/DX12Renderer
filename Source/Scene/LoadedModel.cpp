#include "LoadedModel.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ThirdParty/tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>

using namespace DirectX;

namespace
{
    constexpr int kGltfModeTriangles = 4;

    std::wstring ToWide(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::string DecodeUri(const std::string& uri)
    {
        std::string result;
        result.reserve(uri.size());

        for (size_t i = 0; i < uri.size(); ++i)
        {
            if (uri[i] == '%' && i + 2 < uri.size())
            {
                const auto HexValue = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };

                const int hi = HexValue(uri[i + 1]);
                const int lo = HexValue(uri[i + 2]);

                if (hi >= 0 && lo >= 0)
                {
                    result.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }

            result.push_back(uri[i]);
        }

        return result;
    }

    std::wstring MakeTextureDebugName(
        const std::filesystem::path& path,
        bool srgb)
    {
        std::wstring name =
            srgb ? L"glTF sRGB: " : L"glTF linear: ";

        name += path.filename().wstring();
        return name;
    }

    uint32_t ComponentByteSize(int componentType)
    {
        switch (componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return 1;

        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            return 2;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            return 4;

        default:
            return 0;
        }
    }

    uint32_t ComponentCount(int type)
    {
        switch (type)
        {
        case TINYGLTF_TYPE_SCALAR: return 1;
        case TINYGLTF_TYPE_VEC2:   return 2;
        case TINYGLTF_TYPE_VEC3:   return 3;
        case TINYGLTF_TYPE_VEC4:   return 4;
        default:                   return 0;
        }
    }

    const uint8_t* AccessorData(
        const tinygltf::Model& model,
        const tinygltf::Accessor& accessor)
    {
        if (accessor.bufferView < 0 ||
            accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
        {
            return nullptr;
        }

        const tinygltf::BufferView& view =
            model.bufferViews[accessor.bufferView];

        if (view.buffer < 0 ||
            view.buffer >= static_cast<int>(model.buffers.size()))
        {
            return nullptr;
        }

        const tinygltf::Buffer& buffer =
            model.buffers[view.buffer];

        const size_t offset =
            view.byteOffset + accessor.byteOffset;

        if (offset >= buffer.data.size())
            return nullptr;

        return buffer.data.data() + offset;
    }

    size_t AccessorStride(
        const tinygltf::BufferView& view,
        const tinygltf::Accessor& accessor)
    {
        const int byteStride = accessor.ByteStride(view);

        if (byteStride > 0)
            return static_cast<size_t>(byteStride);

        return static_cast<size_t>(
            ComponentByteSize(accessor.componentType) *
            ComponentCount(accessor.type));
    }

    bool AccessorStorageIsValid(
        const tinygltf::Model& model,
        const tinygltf::Accessor& accessor)
    {
        if (accessor.sparse.isSparse)
            return false;

        if (accessor.bufferView < 0 ||
            accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
        {
            return false;
        }


        const tinygltf::BufferView& view =
            model.bufferViews[accessor.bufferView];

        if (view.buffer < 0 ||
            view.buffer >= static_cast<int>(model.buffers.size()))
        {
            return false;
        }

        const tinygltf::Buffer& buffer =
            model.buffers[view.buffer];

        const uint32_t componentSize =
            ComponentByteSize(accessor.componentType);

        const uint32_t componentCount =
            ComponentCount(accessor.type);

        if (componentSize == 0 || componentCount == 0)
            return false;

        const size_t elementSize =
            static_cast<size_t>(componentSize) *
            static_cast<size_t>(componentCount);

        const size_t stride =
            AccessorStride(view, accessor);

        if (stride < elementSize)
            return false;

        if (accessor.count == 0)
            return false;

        if (accessor.byteOffset > view.byteLength)
            return false;

        const size_t relativeLastElementOffset =
            accessor.byteOffset +
            (accessor.count - 1) * stride;

        if (relativeLastElementOffset < accessor.byteOffset)
            return false;

        const size_t relativeEnd =
            relativeLastElementOffset + elementSize;

        if (relativeEnd < relativeLastElementOffset ||
            relativeEnd > view.byteLength)
        {
            return false;
        }

        const size_t absoluteStart =
            view.byteOffset + accessor.byteOffset;

        const size_t absoluteEnd =
            view.byteOffset + relativeEnd;

        if (absoluteStart < view.byteOffset ||
            absoluteEnd < absoluteStart ||
            absoluteEnd > buffer.data.size())
        {
            return false;
        }

        return true;
    }

    float ReadComponentAsFloat(
        const uint8_t* data,
        int componentType,
        bool normalised)
    {
        switch (componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        {
            const int8_t v = *reinterpret_cast<const int8_t*>(data);
            return normalised
                ? std::max(-1.0f, static_cast<float>(v) / 127.0f)
                : static_cast<float>(v);
        }

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        {
            const uint8_t v = *reinterpret_cast<const uint8_t*>(data);
            return normalised
                ? static_cast<float>(v) / 255.0f
                : static_cast<float>(v);
        }

        case TINYGLTF_COMPONENT_TYPE_SHORT:
        {
            const int16_t v = *reinterpret_cast<const int16_t*>(data);
            return normalised
                ? std::max(-1.0f, static_cast<float>(v) / 32767.0f)
                : static_cast<float>(v);
        }

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        {
            const uint16_t v = *reinterpret_cast<const uint16_t*>(data);
            return normalised
                ? static_cast<float>(v) / 65535.0f
                : static_cast<float>(v);
        }

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(data);
            return static_cast<float>(v);
        }

        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            return *reinterpret_cast<const float*>(data);

        default:
            return 0.0f;
        }
    }

    bool ReadAccessorElement(
        const tinygltf::Model& model,
        int accessorIndex,
        size_t elementIndex,
        float* outValues,
        uint32_t outValueCount)
    {
        if (accessorIndex < 0 ||
            accessorIndex >= static_cast<int>(model.accessors.size()))
        {
            return false;
        }

        const tinygltf::Accessor& accessor =
            model.accessors[accessorIndex];

        if (elementIndex >= accessor.count)
            return false;

        const uint32_t componentCount =
            ComponentCount(accessor.type);

        if (componentCount == 0 ||
            componentCount > outValueCount)
        {
            return false;
        }

        const uint32_t componentSize =
            ComponentByteSize(accessor.componentType);

        if (componentSize == 0)
            return false;

        if (!AccessorStorageIsValid(model, accessor))
            return false;

        const tinygltf::BufferView& view =
            model.bufferViews[accessor.bufferView];

        const uint8_t* base = AccessorData(model, accessor);

        if (!base)
            return false;

        const size_t stride =
            AccessorStride(view, accessor);

        const uint8_t* element =
            base + elementIndex * stride;

        for (uint32_t i = 0; i < componentCount; ++i)
        {
            outValues[i] =
                ReadComponentAsFloat(
                    element + i * componentSize,
                    accessor.componentType,
                    accessor.normalized);
        }

        for (uint32_t i = componentCount; i < outValueCount; ++i)
        {
            outValues[i] = 0.0f;
        }

        return true;
    }

    uint32_t ReadIndexElement(
        const tinygltf::Model& model,
        const tinygltf::Accessor& accessor,
        size_t elementIndex)
    {
        const tinygltf::BufferView& view =
            model.bufferViews[accessor.bufferView];

        const uint8_t* base =
            AccessorData(model, accessor);

        if (!base)
            return 0;

        const size_t stride =
            AccessorStride(view, accessor);

        const uint8_t* element =
            base + elementIndex * stride;

        switch (accessor.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *reinterpret_cast<const uint8_t*>(element);

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            return *reinterpret_cast<const uint16_t*>(element);

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            return *reinterpret_cast<const uint32_t*>(element);

        default:
            return 0;
        }
    }

    bool IsSupportedIndexAccessor(const tinygltf::Accessor& accessor)
    {
        if (accessor.type != TINYGLTF_TYPE_SCALAR)
            return false;

        return accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
            accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ||
            accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    }

    XMMATRIX ReadNodeLocalTransform(const tinygltf::Node& node)
    {
        if (node.matrix.size() == 16)
        {
            // glTF stores column-major matrices for column-vector convention.
            // The renderer uses row vectors with mul(v, M), so this direct row
            // construction gives the equivalent transposed transform.
            return XMMATRIX(
                static_cast<float>(node.matrix[0]),
                static_cast<float>(node.matrix[1]),
                static_cast<float>(node.matrix[2]),
                static_cast<float>(node.matrix[3]),

                static_cast<float>(node.matrix[4]),
                static_cast<float>(node.matrix[5]),
                static_cast<float>(node.matrix[6]),
                static_cast<float>(node.matrix[7]),

                static_cast<float>(node.matrix[8]),
                static_cast<float>(node.matrix[9]),
                static_cast<float>(node.matrix[10]),
                static_cast<float>(node.matrix[11]),

                static_cast<float>(node.matrix[12]),
                static_cast<float>(node.matrix[13]),
                static_cast<float>(node.matrix[14]),
                static_cast<float>(node.matrix[15]));
        }

        XMVECTOR scale =
            XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);

        XMVECTOR rotation =
            XMQuaternionIdentity();

        XMVECTOR translation =
            XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

        if (node.scale.size() == 3)
        {
            scale = XMVectorSet(
                static_cast<float>(node.scale[0]),
                static_cast<float>(node.scale[1]),
                static_cast<float>(node.scale[2]),
                0.0f);
        }

        if (node.rotation.size() == 4)
        {
            rotation = XMVectorSet(
                static_cast<float>(node.rotation[0]),
                static_cast<float>(node.rotation[1]),
                static_cast<float>(node.rotation[2]),
                static_cast<float>(node.rotation[3]));
        }

        if (node.translation.size() == 3)
        {
            translation = XMVectorSet(
                static_cast<float>(node.translation[0]),
                static_cast<float>(node.translation[1]),
                static_cast<float>(node.translation[2]),
                1.0f);
        }

        // Row-vector equivalent of glTF TRS.
        return
            XMMatrixScalingFromVector(scale) *
            XMMatrixRotationQuaternion(rotation) *
            XMMatrixTranslationFromVector(translation);
    }

    int FindAttribute(
        const tinygltf::Primitive& primitive,
        const char* name)
    {
        const auto it = primitive.attributes.find(name);

        if (it == primitive.attributes.end())
            return -1;

        return it->second;
    }

    LoadedModel::Bounds MakeInvalidBounds()
    {
        LoadedModel::Bounds b{};
        b.min =
        {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };

        b.max =
        {
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()
        };

        b.center = { 0.0f, 0.0f, 0.0f };
        b.extent = { 0.0f, 0.0f, 0.0f };
        b.valid = false;
        return b;
    }

    void IncludePoint(
        LoadedModel::Bounds& bounds,
        DirectX::XMFLOAT3 p)
    {
        if (!bounds.valid)
        {
            bounds.min = p;
            bounds.max = p;
            bounds.valid = true;
            return;
        }

        bounds.min.x = std::min(bounds.min.x, p.x);
        bounds.min.y = std::min(bounds.min.y, p.y);
        bounds.min.z = std::min(bounds.min.z, p.z);

        bounds.max.x = std::max(bounds.max.x, p.x);
        bounds.max.y = std::max(bounds.max.y, p.y);
        bounds.max.z = std::max(bounds.max.z, p.z);
    }

    void IncludeBounds(
        LoadedModel::Bounds& dst,
        const LoadedModel::Bounds& src)
    {
        if (!src.valid)
            return;

        IncludePoint(dst, src.min);
        IncludePoint(dst, src.max);
    }

    void FinaliseBounds(LoadedModel::Bounds& bounds)
    {
        if (!bounds.valid)
            return;

        bounds.center =
        {
            0.5f * (bounds.min.x + bounds.max.x),
            0.5f * (bounds.min.y + bounds.max.y),
            0.5f * (bounds.min.z + bounds.max.z)
        };

        bounds.extent =
        {
            0.5f * (bounds.max.x - bounds.min.x),
            0.5f * (bounds.max.y - bounds.min.y),
            0.5f * (bounds.max.z - bounds.min.z)
        };
    }

    LoadedModel::Bounds TransformBounds(
        const LoadedModel::Bounds& localBounds,
        DirectX::FXMMATRIX transform)
    {
        LoadedModel::Bounds worldBounds =
            MakeInvalidBounds();

        if (!localBounds.valid)
            return worldBounds;

        const DirectX::XMFLOAT3 mn = localBounds.min;
        const DirectX::XMFLOAT3 mx = localBounds.max;

        const DirectX::XMFLOAT3 corners[8] =
        {
            { mn.x, mn.y, mn.z },
            { mx.x, mn.y, mn.z },
            { mn.x, mx.y, mn.z },
            { mx.x, mx.y, mn.z },
            { mn.x, mn.y, mx.z },
            { mx.x, mn.y, mx.z },
            { mn.x, mx.y, mx.z },
            { mx.x, mx.y, mx.z }
        };

        for (const DirectX::XMFLOAT3& corner : corners)
        {
            const DirectX::XMVECTOR p =
                DirectX::XMLoadFloat3(&corner);

            DirectX::XMFLOAT3 transformed{};
            DirectX::XMStoreFloat3(
                &transformed,
                DirectX::XMVector3TransformCoord(p, transform));

            IncludePoint(worldBounds, transformed);
        }

        FinaliseBounds(worldBounds);
        return worldBounds;
    }

    bool MatrixReversesWinding(DirectX::FXMMATRIX m)
    {
        using namespace DirectX;

        const XMVECTOR det = XMMatrixDeterminant(m);
        return XMVectorGetX(det) < -1.0e-6f;
    }
}

void LoadedModel::Clear()
{
    m_mesh = Mesh{};
    m_materials.clear();
    m_textures.clear();
    m_draws.clear();
    m_lastError.clear();
    m_bounds = {};
    m_stats = {};
    m_loaded = false;
}

Material* LoadedModel::GetMaterial(uint32_t index)
{
    if (m_materials.empty())
        return nullptr;

    if (index >= m_materials.size())
        index = 0;

    return &m_materials[index];
}

const Material* LoadedModel::GetMaterial(uint32_t index) const
{
    if (m_materials.empty())
        return nullptr;

    if (index >= m_materials.size())
        index = 0;

    return &m_materials[index];
}

bool LoadedModel::LoadGltf(
    ID3D12Device* device,
    CommandList& cl,
    UploadArena& upload,
    DescriptorAllocator& srvHeap,
    uint32_t frameIndex,
    const std::filesystem::path& path,
    const XMMATRIX& importTransform)
{
    Clear();

    if (!device)
    {
        m_lastError = L"LoadedModel::LoadGltf failed: null D3D12 device.";
        return false;
    }

    if (!std::filesystem::exists(path))
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: file does not exist: " +
            path.wstring();

        return false;
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string warning;
    std::string error;

    const std::string pathString = path.string();

    bool loaded = false;

    const std::filesystem::path extension = path.extension();

    if (extension == L".glb")
    {
        loaded =
            loader.LoadBinaryFromFile(
                &model,
                &error,
                &warning,
                pathString);
    }
    else
    {
        loaded =
            loader.LoadASCIIFromFile(
                &model,
                &error,
                &warning,
                pathString);
    }

    if (!warning.empty())
    {
        OutputDebugStringW((L"glTF warning: " + ToWide(warning) + L"\n").c_str());
    }

    if (!loaded)
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: " +
            ToWide(error);

        return false;
    }

    if (!model.extensionsRequired.empty())
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed.";

        return false;
    }

    const std::filesystem::path baseDir =
        path.parent_path();

    std::vector<Mesh::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Mesh::Submesh> submeshes;
    std::vector<Bounds> submeshLocalBounds;

    std::vector<std::vector<uint32_t>> meshPrimitiveToSubmesh;
    meshPrimitiveToSubmesh.resize(model.meshes.size());

    bool needsGeneratedTangents = false;

    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
    {
        const tinygltf::Mesh& gltfMesh = model.meshes[meshIndex];

        meshPrimitiveToSubmesh[meshIndex].resize(
            gltfMesh.primitives.size(),
            UINT32_MAX);

        for (size_t primitiveIndex = 0; primitiveIndex < gltfMesh.primitives.size(); ++primitiveIndex)
        {
            const tinygltf::Primitive& primitive =
                gltfMesh.primitives[primitiveIndex];

            if (primitive.mode != kGltfModeTriangles)
            {
                m_lastError =
                    L"LoadedModel::LoadGltf failed";

                return false;
            }

            const int positionAccessor = FindAttribute(primitive, "POSITION");
            const int normalAccessor = FindAttribute(primitive, "NORMAL");
            const int tangentAccessor = FindAttribute(primitive, "TANGENT");
            const int uvAccessor = FindAttribute(primitive, "TEXCOORD_0");
            const int colourAccessor = FindAttribute(primitive, "COLOR_0");
            const int texCoord1Accessor = FindAttribute(primitive, "TEXCOORD_1");

            if (positionAccessor < 0)
            {
                m_lastError =
                    L"LoadedModel::LoadGltf failed: primitive is missing POSITION.";

                return false;
            }

            const tinygltf::Accessor& posAccessor =
                model.accessors[positionAccessor];

            if (posAccessor.type != TINYGLTF_TYPE_VEC3 ||
                posAccessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                m_lastError =
                    L"LoadedModel::LoadGltf failed: POSITION must be FLOAT VEC3.";

                return false;
            }

            const uint32_t vertexOffset =
                static_cast<uint32_t>(vertices.size());

            const uint32_t vertexCount =
                static_cast<uint32_t>(posAccessor.count);

            vertices.resize(vertices.size() + vertexCount);

            if (tangentAccessor < 0)
            {
                needsGeneratedTangents = true;
            }

            Bounds primitiveBounds = MakeInvalidBounds();

            for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                Mesh::Vertex& v =
                    vertices[vertexOffset + vertexIndex];

                float values[4] = {};

                if (!ReadAccessorElement(model, positionAccessor, vertexIndex, values, 4))
                {
                    m_lastError =
                        L"LoadedModel::LoadGltf failed while reading POSITION.";

                    return false;
                }

                v.px = values[0];
                v.py = values[1];
                v.pz = values[2];


                IncludePoint(
                    primitiveBounds,
                    { v.px, v.py, v.pz });

                v.nx = 0.0f;
                v.ny = 1.0f;
                v.nz = 0.0f;

                if (normalAccessor >= 0 &&
                    ReadAccessorElement(model, normalAccessor, vertexIndex, values, 4))
                {
                    v.nx = values[0];
                    v.ny = values[1];
                    v.nz = values[2];
                }

                v.tx = 1.0f;
                v.ty = 0.0f;
                v.tz = 0.0f;
                v.tw = 1.0f;

                if (tangentAccessor >= 0 &&
                    ReadAccessorElement(model, tangentAccessor, vertexIndex, values, 4))
                {
                    v.tx = values[0];
                    v.ty = values[1];
                    v.tz = values[2];
                    v.tw = values[3];
                }

                v.u = 0.0f;
                v.v = 0.0f;

                if (uvAccessor >= 0 &&
                    ReadAccessorElement(model, uvAccessor, vertexIndex, values, 4))
                {
                    v.u = values[0];
                    v.v = values[1];
                }

                v.r = 1.0f;
                v.g = 1.0f;
                v.b = 1.0f;
                v.a = 1.0f;

                if (colourAccessor >= 0 &&
                    ReadAccessorElement(model, colourAccessor, vertexIndex, values, 4))
                {
                    v.r = values[0];
                    v.g = values[1];
                    v.b = values[2];
                    v.a = values[3] == 0.0f ? 1.0f : values[3];
                }

                v.u1 = v.u;
                v.v1 = v.v;

                if (texCoord1Accessor >= 0)
                {
                    float uv1[4] = {};

                    if (!ReadAccessorElement(model, texCoord1Accessor, vertexIndex, uv1, 4))
                    {
                        m_lastError =
                            L"LoadedModel::LoadGltf failed while reading TEXCOORD_1.";

                        return false;
                    }

                    v.u1 = uv1[0];
                    v.v1 = uv1[1];
                }
            }

            FinaliseBounds(primitiveBounds);

            const uint32_t indexStart =
                static_cast<uint32_t>(indices.size());

            if (primitive.indices >= 0)
            {
                if (primitive.indices >= static_cast<int>(model.accessors.size()))
                {
                    m_lastError =
                        L"LoadedModel::LoadGltf failed: invalid index accessor.";

                    return false;
                }


                const tinygltf::Accessor& indexAccessor =
                    model.accessors[primitive.indices];

                if (!AccessorStorageIsValid(model, indexAccessor))
                {
                    m_lastError =
                        L"LoadedModel::LoadGltf failed: invalid index accessor storage.";

                    return false;
                }

                if (!IsSupportedIndexAccessor(indexAccessor))
                {
                    m_lastError =
                        L"LoadedModel::LoadGltf failed: unsupported index accessor type.";

                    return false;
                }

                for (size_t indexIndex = 0; indexIndex < indexAccessor.count; ++indexIndex)
                {
                    const uint32_t localIndex =
                        ReadIndexElement(
                            model,
                            indexAccessor,
                            indexIndex);

                    if (localIndex >= vertexCount)
                    {
                        m_lastError =
                            L"LoadedModel::LoadGltf failed: index exceeds primitive vertex count.";

                        return false;
                    }

                    indices.push_back(vertexOffset + localIndex);
                }
            }
            else
            {
                for (uint32_t i = 0; i < vertexCount; ++i)
                {
                    indices.push_back(vertexOffset + i);
                }
            }

            const uint32_t indexCount =
                static_cast<uint32_t>(indices.size()) - indexStart;

            if ((indexCount % 3) != 0)
            {
                m_lastError =
                    L"LoadedModel::LoadGltf failed: primitive index count is not triangle-aligned.";

                return false;
            }

            uint32_t materialIndex = 0;

            if (primitive.material >= 0 &&
                primitive.material < static_cast<int>(model.materials.size()))
            {
                materialIndex =
                    static_cast<uint32_t>(primitive.material);
            }

            Mesh::Submesh submesh{};
            submesh.indexStart = indexStart;
            submesh.indexCount = indexCount;
            submesh.vertexBase = 0;
            submesh.materialIndex = materialIndex;

            const uint32_t submeshIndex =
                static_cast<uint32_t>(submeshes.size());

            submeshes.push_back(submesh);
            submeshLocalBounds.push_back(primitiveBounds);

            meshPrimitiveToSubmesh[meshIndex][primitiveIndex] =
                submeshIndex;
        }
    }

    if (vertices.empty() || indices.empty() || submeshes.empty())
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: model produced no drawable geometry.";

        return false;
    }

    std::vector<Draw> loadedDraws;
    Bounds loadedBounds = MakeInvalidBounds();

    const int sceneIndex =
        (model.defaultScene >= 0 &&
            model.defaultScene < static_cast<int>(model.scenes.size()))
        ? model.defaultScene
        : 0;

    if (sceneIndex < 0 ||
        sceneIndex >= static_cast<int>(model.scenes.size()))
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: model has no valid scene.";

        return false;
    }

    const tinygltf::Scene& scene =
        model.scenes[sceneIndex];

    std::function<void(int, XMMATRIX)> TraverseNode;

    TraverseNode = [&](int nodeIndex, XMMATRIX parentWorld)
    {
        if (nodeIndex < 0 ||
            nodeIndex >= static_cast<int>(model.nodes.size()))
        {
            return;
        }

        const tinygltf::Node& node =
            model.nodes[nodeIndex];

        const XMMATRIX local =
            ReadNodeLocalTransform(node);

        const XMMATRIX world =
            local * parentWorld;

        if (node.mesh >= 0 &&
            node.mesh < static_cast<int>(model.meshes.size()))
        {
            const tinygltf::Mesh& gltfMesh =
                model.meshes[node.mesh];

            for (size_t primitiveIndex = 0;
                primitiveIndex < gltfMesh.primitives.size();
                ++primitiveIndex)
            {
                const uint32_t submeshIndex =
                    meshPrimitiveToSubmesh[node.mesh][primitiveIndex];

                if (submeshIndex == UINT32_MAX ||
                    submeshIndex >= submeshes.size())
                {
                    continue;
                }

                const XMMATRIX drawWorld =
                    world * importTransform;

                Draw draw{};
                draw.submeshIndex = submeshIndex;
                draw.materialIndex = submeshes[submeshIndex].materialIndex;
                draw.reversesWinding = MatrixReversesWinding(drawWorld);

                XMStoreFloat4x4(
                    &draw.world,
                    drawWorld);

                loadedDraws.push_back(draw);

                const Bounds worldSubmeshBounds =
                    TransformBounds(
                        submeshLocalBounds[submeshIndex],
                        world * importTransform);

                IncludeBounds(
                    loadedBounds,
                    worldSubmeshBounds);
            }
        }

        for (int childIndex : node.children)
        {
            TraverseNode(childIndex, world);
        }
    };

    for (int rootNode : scene.nodes)
    {
        TraverseNode(rootNode, XMMatrixIdentity());
    }

    if (loadedDraws.empty())
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: scene produced no draw items.";

        return false;
    }

    FinaliseBounds(loadedBounds);

    m_mesh.CreateFromData(
        device,
        cl,
        upload,
        frameIndex,
        vertices,
        indices,
        submeshes,
        path.filename().c_str(),
        needsGeneratedTangents);

    if (m_mesh.IndexCount() == 0)
    {
        m_lastError =
            L"LoadedModel::LoadGltf failed: mesh upload rejected imported data.";

        return false;
    }

    struct TextureCacheKey
    {
        int textureIndex = -1;
        bool srgb = false;

        bool operator==(const TextureCacheKey& rhs) const
        {
            return textureIndex == rhs.textureIndex &&
                srgb == rhs.srgb;
        }
    };

    struct TextureCacheKeyHash
    {
        size_t operator()(const TextureCacheKey& key) const
        {
            return
                (static_cast<size_t>(key.textureIndex) << 1) ^
                (key.srgb ? 1u : 0u);
        }
    };

    std::unordered_map<TextureCacheKey, uint32_t, TextureCacheKeyHash> textureCache;
    m_textures.reserve(model.textures.size() * 2);

    auto LoadTexture = [&](int textureIndex, bool srgb, const std::wstring& usage) -> int32_t
    {
        if (textureIndex < 0 ||
            textureIndex >= static_cast<int>(model.textures.size()))
        {
            return -1;
        }

        const TextureCacheKey key{ textureIndex, srgb };

        const auto found = textureCache.find(key);

        if (found != textureCache.end())
        {
            return static_cast<int32_t>(found->second);
        }

        const tinygltf::Texture& gltfTexture =
            model.textures[textureIndex];

        if (gltfTexture.source < 0 ||
            gltfTexture.source >= static_cast<int>(model.images.size()))
        {
            return -1;
        }

        const tinygltf::Image& image =
            model.images[gltfTexture.source];

        if (image.uri.empty())
        {
            return -1;
        }

        if (image.uri.rfind("data:", 0) == 0)
        {
            return -1;
        }

        const std::filesystem::path texturePath =
            (baseDir / std::filesystem::path(DecodeUri(image.uri))).lexically_normal();

        if (!std::filesystem::exists(texturePath))
        {
            OutputDebugStringW(
                (L"glTF texture missing: " + texturePath.wstring() + L"\n").c_str());

            return -1;
        }

        const uint32_t newTextureIndex =
            static_cast<uint32_t>(m_textures.size());

        m_textures.emplace_back();

        const std::wstring debugName =
            MakeTextureDebugName(texturePath, srgb);

        std::wstring error;

        if (!m_textures.back().TryLoadFromFile_DirectXTex(
            device,
            cl,
            upload,
            frameIndex,
            texturePath,
            srgb,
            debugName.c_str(),
            &error))
        {
            OutputDebugStringW(
                (L"glTF texture skipped for " +
                    usage +
                    L": " +
                    error +
                    L"\n").c_str());

            m_textures.pop_back();
            return -1;
        }

        textureCache[key] = newTextureIndex;
        return static_cast<int32_t>(newTextureIndex);
    };

    struct MaterialTextureRefs
    {
        int32_t baseColor = -1;
        int32_t normal = -1;
        int32_t metallicRoughness = -1;
        int32_t occlusion = -1;
        int32_t emissive = -1;
    };

    const size_t materialCount =
        std::max<size_t>(1, model.materials.size());

    m_materials.resize(materialCount);

    std::vector<MaterialTextureRefs> materialTextures;
    materialTextures.resize(materialCount);

    for (size_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        Material& material =
            m_materials[materialIndex];

        MaterialTextureRefs& refs =
            materialTextures[materialIndex];

        if (materialIndex >= model.materials.size())
        {
            material.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
            material.metallicFactor = 0.0f;
            material.roughnessFactor = 0.5f;
            material.occlusionStrength = 1.0f;
            continue;
        }

        const tinygltf::Material& gltfMaterial =
            model.materials[materialIndex];

        const tinygltf::PbrMetallicRoughness& pbr =
            gltfMaterial.pbrMetallicRoughness;

        if (pbr.baseColorFactor.size() == 4)
        {
            material.baseColorFactor =
            {
                static_cast<float>(pbr.baseColorFactor[0]),
                static_cast<float>(pbr.baseColorFactor[1]),
                static_cast<float>(pbr.baseColorFactor[2]),
                static_cast<float>(pbr.baseColorFactor[3])
            };
        }
        else
        {
            material.baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
        }

        material.metallicFactor =
            static_cast<float>(pbr.metallicFactor);

        material.roughnessFactor =
            static_cast<float>(pbr.roughnessFactor);

        material.occlusionStrength =
            std::clamp(
                static_cast<float>(gltfMaterial.occlusionTexture.strength),
                0.0f,
                1.0f);

        material.normalScale =
            static_cast<float>(gltfMaterial.normalTexture.scale);

        material.occlusionStrength =
            std::clamp(
                static_cast<float>(gltfMaterial.occlusionTexture.strength),
                0.0f,
                1.0f);

        material.alphaCutoff =
            static_cast<float>(
                gltfMaterial.alphaCutoff > 0.0
                ? gltfMaterial.alphaCutoff
                : 0.5);

        material.doubleSided =
            gltfMaterial.doubleSided ? 1u : 0u;

        material.emissiveFactor = { 0.0f, 0.0f, 0.0f, 0.0f };

        if (gltfMaterial.emissiveFactor.size() >= 3)
        {
            material.emissiveFactor =
            {
                static_cast<float>(gltfMaterial.emissiveFactor[0]),
                static_cast<float>(gltfMaterial.emissiveFactor[1]),
                static_cast<float>(gltfMaterial.emissiveFactor[2]),
                0.0f
            };
        }

        material.baseColorTexCoord =
            static_cast<uint32_t>(
                std::max(0, pbr.baseColorTexture.texCoord));

        material.metalRoughTexCoord =
            static_cast<uint32_t>(
                std::max(0, pbr.metallicRoughnessTexture.texCoord));

        material.normalTexCoord =
            static_cast<uint32_t>(
                std::max(0, gltfMaterial.normalTexture.texCoord));

        material.occlusionTexCoord =
            static_cast<uint32_t>(
                std::max(0, gltfMaterial.occlusionTexture.texCoord));

        material.emissiveTexCoord =
            static_cast<uint32_t>(
                std::max(0, gltfMaterial.emissiveTexture.texCoord));

        std::wstring materialLabel =
            L"material[" +
            std::to_wstring(materialIndex) +
            L"]";

        if (!gltfMaterial.name.empty())
        {
            materialLabel += L" ";
            materialLabel += ToWide(gltfMaterial.name);
        }

        if (gltfMaterial.alphaMode == "MASK")
        {
            material.alphaMode = MaterialAlphaMode::Mask;
        }
        else if (gltfMaterial.alphaMode == "BLEND")
        {
            material.alphaMode = MaterialAlphaMode::Blend;

            OutputDebugStringW(
                (L"glTF material " +
                    materialLabel +
                    L" uses alphaMode=BLEND; Phase 3E records it but renders it as opaque until sorted transparency is implemented.\n").c_str());
        }
        else
        {
            material.alphaMode = MaterialAlphaMode::Opaque;
        }

        auto ClampTexCoordSet = [&](uint32_t& set, const wchar_t* usage)
        {
            if (set > 1u)
            {
                OutputDebugStringW(
                    (L"glTF material " +
                        materialLabel +
                        L" uses unsupported " +
                        usage +
                        L" TEXCOORD_" +
                        std::to_wstring(set) +
                        L"; clamping to TEXCOORD_1.\n").c_str());

                set = 1u;
            }
        };

        ClampTexCoordSet(material.baseColorTexCoord, L"baseColor");
        ClampTexCoordSet(material.normalTexCoord, L"normal");
        ClampTexCoordSet(material.metalRoughTexCoord, L"metallicRoughness");
        ClampTexCoordSet(material.occlusionTexCoord, L"occlusion");
        ClampTexCoordSet(material.emissiveTexCoord, L"emissive");

        refs.baseColor =
            LoadTexture(
                pbr.baseColorTexture.index,
                true,
                materialLabel + L" baseColor");

        refs.metallicRoughness =
            LoadTexture(
                pbr.metallicRoughnessTexture.index,
                false,
                materialLabel + L" metallicRoughness");

        refs.normal =
            LoadTexture(
                gltfMaterial.normalTexture.index,
                false,
                materialLabel + L" normal");

        if (gltfMaterial.occlusionTexture.index >= 0 &&
            gltfMaterial.occlusionTexture.texCoord != 0)
        {
            OutputDebugStringW(
                (L"glTF occlusion texture on " +
                    materialLabel +
                    L" uses TEXCOORD_" +
                    std::to_wstring(gltfMaterial.occlusionTexture.texCoord) +
                    L"; using declared TEXCOORD set where supported.\n").c_str());
        }

        refs.occlusion =
            LoadTexture(
                gltfMaterial.occlusionTexture.index,
                false,
                materialLabel + L" occlusion");

        refs.emissive =
            LoadTexture(
                gltfMaterial.emissiveTexture.index,
                true,
                materialLabel + L" emissive");
    }

    auto TexturePtr = [&](int32_t textureIndex) -> const Texture*
    {
        if (textureIndex < 0 ||
            textureIndex >= static_cast<int32_t>(m_textures.size()))
        {
            return nullptr;
        }

        return &m_textures[textureIndex];
    };

    for (size_t materialIndex = 0; materialIndex < m_materials.size(); ++materialIndex)
    {
        const MaterialTextureRefs& refs =
            materialTextures[materialIndex];

        m_materials[materialIndex].UpdateDescriptorTable(
            device,
            srvHeap,
            TexturePtr(refs.baseColor),
            TexturePtr(refs.normal),
            TexturePtr(refs.metallicRoughness),
            TexturePtr(refs.occlusion),
            TexturePtr(refs.emissive));
    }
 

    m_draws = std::move(loadedDraws);
    m_bounds = loadedBounds;

    m_stats.vertexCount =
        static_cast<uint32_t>(vertices.size());

    m_stats.indexCount =
        static_cast<uint32_t>(indices.size());

    m_stats.triangleCount =
        static_cast<uint32_t>(indices.size() / 3);

    m_stats.submeshCount =
        static_cast<uint32_t>(submeshes.size());

    m_stats.drawCount =
        static_cast<uint32_t>(m_draws.size());

    m_stats.materialCount =
        static_cast<uint32_t>(m_materials.size());

    m_stats.textureCount =
        static_cast<uint32_t>(m_textures.size());

    m_loaded = true;
    return true;
}
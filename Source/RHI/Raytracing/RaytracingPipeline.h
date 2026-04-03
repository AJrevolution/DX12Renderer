#pragma once
#include "Common.h"
#include <filesystem>

class RaytracingPipeline
{
public:
    void Initialize(ID3D12Device5* device, const std::filesystem::path& shaderDir);

    ID3D12StateObject* StateObject() const { return m_stateObject.Get(); }
    ID3D12RootSignature* GlobalRootSignature() const { return m_globalRootSig.Get(); }
    ID3D12Resource* ShaderTable() const { return m_shaderTable.Get(); }

    uint32_t RayGenOffset() const { return m_rayGenOffset; }
    uint32_t MissOffset() const { return m_missOffset; }
    uint32_t HitGroupOffset() const { return m_hitOffset; }

    uint32_t RayGenRecordSize() const { return m_rayGenRecordSize; }
    uint32_t MissRecordSize() const { return m_missRecordSize; }
    uint32_t HitGroupRecordSize() const { return m_hitRecordSize; }

private:
    void BuildRootSignature(ID3D12Device* device);
    void BuildStateObject(ID3D12Device5* device, const std::filesystem::path& shaderPath);
    void BuildShaderTable(ID3D12Device5* device);

private:
    ComPtr<ID3D12RootSignature> m_globalRootSig;
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_stateObjectProps;
    ComPtr<ID3D12Resource> m_shaderTable;

    uint32_t m_rayGenRecordSize = 0;
    uint32_t m_missRecordSize = 0;
    uint32_t m_hitRecordSize = 0;

    uint32_t m_rayGenOffset = 0;
    uint32_t m_missOffset = 0;
    uint32_t m_hitOffset = 0;
};
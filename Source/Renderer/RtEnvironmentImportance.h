#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

struct RtEnvAliasEntry
{
    float q = 1.0f;
    uint32_t alias = 0;
    float pdf = 0.0f;     // Final solid-angle PDF for this cubemap texel.
    float weight = 0.0f;  // Debug luminance / importance weight.
};
static_assert(sizeof(RtEnvAliasEntry) == 16, "RtEnvAliasEntry must match the HLSL layout.");

class RtEnvironmentImportance
{
public:
    bool BuildFromCubeFaces(
        const std::vector<DirectX::XMFLOAT3>& radiance,
        uint32_t faceSize);

    bool BuildUniformFallback(uint32_t faceSize);

    bool Validate(std::string* error = nullptr) const;

    const std::vector<RtEnvAliasEntry>& Entries() const { return m_entries; }

    uint32_t FaceSize() const { return m_faceSize; }
    uint32_t AliasCount() const { return static_cast<uint32_t>(m_entries.size()); }
    float TotalWeight() const { return m_totalWeight; }
    bool IsFallback() const { return m_fallback; }

private:
    static float CubeTexelSolidAngle(
        uint32_t x,
        uint32_t y,
        uint32_t faceSize);

    static void BuildAliasTable(
        const std::vector<float>& probabilities,
        const std::vector<float>& solidAngle,
        std::vector<RtEnvAliasEntry>& out);

    uint32_t m_faceSize = 0;
    float m_totalWeight = 0.0f;
    bool m_fallback = true;
    std::vector<RtEnvAliasEntry> m_entries;
};

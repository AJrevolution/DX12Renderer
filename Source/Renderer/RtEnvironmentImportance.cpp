#include "RtEnvironmentImportance.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    bool CheckedCubeEntryCount(uint32_t faceSize, size_t& outCount)
    {
        outCount = 0;

        if (faceSize == 0)
            return false;

        const uint64_t fs = static_cast<uint64_t>(faceSize);

        // Alias indices are uint32_t, so keep the table count representable.
        if (fs > (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / 6ull) / fs)
            return false;

        const uint64_t count64 = 6ull * fs * fs;

        if (count64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
            return false;

        outCount = static_cast<size_t>(count64);
        return true;
    }

    float SaturateFiniteNonNegative(float v)
    {
        if (!std::isfinite(v) || v <= 0.0f)
            return 0.0f;

        return v;
    }

    float LuminanceRec709(const DirectX::XMFLOAT3& c)
    {
        const float r = SaturateFiniteNonNegative(c.x);
        const float g = SaturateFiniteNonNegative(c.y);
        const float b = SaturateFiniteNonNegative(c.z);

        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    float ClampToFloat(double v)
    {
        if (!std::isfinite(v) || v <= 0.0)
            return 0.0f;

        const double maxFloat =
            static_cast<double>(std::numeric_limits<float>::max());

        return static_cast<float>(std::min(v, maxFloat));
    }

    void ClearInvalidState(
        uint32_t& faceSize,
        float& totalWeight,
        bool& fallback,
        std::vector<RtEnvAliasEntry>& entries)
    {
        faceSize = 0;
        totalWeight = 0.0f;
        fallback = true;
        entries.clear();
    }
}

bool RtEnvironmentImportance::BuildFromCubeFaces(
    const std::vector<DirectX::XMFLOAT3>& radiance,
    uint32_t faceSize)
{
    size_t count = 0;
    if (!CheckedCubeEntryCount(faceSize, count))
    {
        ClearInvalidState(m_faceSize, m_totalWeight, m_fallback, m_entries);
        return false;
    }

    if (radiance.size() != count)
    {
        ClearInvalidState(m_faceSize, m_totalWeight, m_fallback, m_entries);
        return false;
    }

    std::vector<float> solidAngle(count, 0.0f);
    std::vector<double> weights(count, 0.0);
    std::vector<float> probabilities(count, 0.0f);

    double totalWeight = 0.0;

    const uint32_t faceTexelCount = faceSize * faceSize;

    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t y = 0; y < faceSize; ++y)
        {
            for (uint32_t x = 0; x < faceSize; ++x)
            {
                const size_t index =
                    static_cast<size_t>(face) * faceTexelCount +
                    static_cast<size_t>(y) * faceSize +
                    x;

                const float sa = CubeTexelSolidAngle(x, y, faceSize);
                solidAngle[index] = sa;

                const float lum = LuminanceRec709(radiance[index]);

                const double w =
                    static_cast<double>(lum) *
                    static_cast<double>(sa);

                if (std::isfinite(w) && w > 0.0)
                {
                    weights[index] = w;
                    totalWeight += w;
                }
            }
        }
    }

    // No useful CPU environment radiance: use a uniform solid-angle fallback.
    if (!std::isfinite(totalWeight) || totalWeight <= 0.0)
        return BuildUniformFallback(faceSize);

    double probabilitySum = 0.0;

    for (size_t i = 0; i < count; ++i)
    {
        const double p = weights[i] / totalWeight;

        if (std::isfinite(p) && p > 0.0)
        {
            probabilities[i] = static_cast<float>(p);
            probabilitySum += static_cast<double>(probabilities[i]);
        }
    }

    if (!std::isfinite(probabilitySum) || probabilitySum <= 0.0)
        return BuildUniformFallback(faceSize);

    // Renormalize after float conversion so the alias builder sees a clean PMF.
    for (float& p : probabilities)
        p = static_cast<float>(static_cast<double>(p) / probabilitySum);

    BuildAliasTable(probabilities, solidAngle, m_entries);

    for (size_t i = 0; i < count; ++i)
        m_entries[i].weight = ClampToFloat(weights[i]);

    m_faceSize = faceSize;
    m_totalWeight = ClampToFloat(totalWeight);
    m_fallback = false;

    if (!Validate(nullptr))
        return BuildUniformFallback(faceSize);

    return true;
}

bool RtEnvironmentImportance::BuildUniformFallback(uint32_t faceSize)
{
    size_t count = 0;
    if (!CheckedCubeEntryCount(faceSize, count))
    {
        ClearInvalidState(m_faceSize, m_totalWeight, m_fallback, m_entries);
        return false;
    }

    std::vector<float> solidAngle(count, 0.0f);
    std::vector<float> probabilities(count, 0.0f);

    double totalSolidAngle = 0.0;

    const uint32_t faceTexelCount = faceSize * faceSize;

    for (uint32_t face = 0; face < 6; ++face)
    {
        for (uint32_t y = 0; y < faceSize; ++y)
        {
            for (uint32_t x = 0; x < faceSize; ++x)
            {
                const size_t index =
                    static_cast<size_t>(face) * faceTexelCount +
                    static_cast<size_t>(y) * faceSize +
                    x;

                const float sa = CubeTexelSolidAngle(x, y, faceSize);
                solidAngle[index] = sa;
                totalSolidAngle += static_cast<double>(sa);
            }
        }
    }

    if (!std::isfinite(totalSolidAngle) || totalSolidAngle <= 0.0)
    {
        ClearInvalidState(m_faceSize, m_totalWeight, m_fallback, m_entries);
        return false;
    }

    double probabilitySum = 0.0;

    for (size_t i = 0; i < count; ++i)
    {
        const double p =
            static_cast<double>(solidAngle[i]) /
            totalSolidAngle;

        probabilities[i] = static_cast<float>(p);
        probabilitySum += static_cast<double>(probabilities[i]);
    }

    if (!std::isfinite(probabilitySum) || probabilitySum <= 0.0)
    {
        ClearInvalidState(m_faceSize, m_totalWeight, m_fallback, m_entries);
        return false;
    }

    for (float& p : probabilities)
        p = static_cast<float>(static_cast<double>(p) / probabilitySum);

    BuildAliasTable(probabilities, solidAngle, m_entries);

    for (size_t i = 0; i < count; ++i)
        m_entries[i].weight = solidAngle[i];

    m_faceSize = faceSize;
    m_totalWeight = ClampToFloat(totalSolidAngle);
    m_fallback = true;

    return Validate(nullptr);
}

bool RtEnvironmentImportance::Validate(std::string* error) const
{
    auto Fail = [&](const char* msg) -> bool
    {
        if (error)
            *error = msg;

        return false;
    };

    if (m_faceSize == 0)
        return Fail("RtEnvironmentImportance has zero face size.");

    size_t expectedCount = 0;
    if (!CheckedCubeEntryCount(m_faceSize, expectedCount))
        return Fail("RtEnvironmentImportance face size overflows alias table count.");

    if (m_entries.size() != expectedCount)
        return Fail("RtEnvironmentImportance alias table size does not match face size.");

    if (!std::isfinite(m_totalWeight) || m_totalWeight <= 0.0f)
        return Fail("RtEnvironmentImportance total weight is invalid.");

    const size_t faceTexelCount =
        static_cast<size_t>(m_faceSize) *
        static_cast<size_t>(m_faceSize);

    double pdfIntegral = 0.0;

    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const RtEnvAliasEntry& e = m_entries[i];

        if (!std::isfinite(e.q) || e.q < -1e-5f || e.q > 1.00001f)
            return Fail("RtEnvironmentImportance alias q is outside [0, 1].");

        if (e.alias >= m_entries.size())
            return Fail("RtEnvironmentImportance alias index is out of range.");

        if (!std::isfinite(e.pdf) || e.pdf < 0.0f)
            return Fail("RtEnvironmentImportance PDF is invalid.");

        if (!std::isfinite(e.weight) || e.weight < 0.0f)
            return Fail("RtEnvironmentImportance weight is invalid.");

        const size_t face = i / faceTexelCount;
        const size_t rem = i - face * faceTexelCount;
        const uint32_t y = static_cast<uint32_t>(rem / m_faceSize);
        const uint32_t x = static_cast<uint32_t>(rem - static_cast<size_t>(y) * m_faceSize);

        const float sa = CubeTexelSolidAngle(x, y, m_faceSize);
        pdfIntegral += static_cast<double>(e.pdf) * static_cast<double>(sa);
    }

    if (!std::isfinite(pdfIntegral))
        return Fail("RtEnvironmentImportance PDF integral is not finite.");

    if (std::fabs(pdfIntegral - 1.0) > 5e-3)
        return Fail("RtEnvironmentImportance PDF does not integrate to approximately 1.");

    if (error)
        error->clear();

    return true;
}

float RtEnvironmentImportance::CubeTexelSolidAngle(
    uint32_t x,
    uint32_t y,
    uint32_t faceSize)
{
    if (faceSize == 0)
        return 0.0f;

    const float fs = static_cast<float>(faceSize);

    const float du = 2.0f / fs;
    const float dv = 2.0f / fs;

    const float u = ((static_cast<float>(x) + 0.5f) / fs) * 2.0f - 1.0f;
    const float v = ((static_cast<float>(y) + 0.5f) / fs) * 2.0f - 1.0f;

    const float denom = std::pow(1.0f + u * u + v * v, 1.5f);
    const float solidAngle = du * dv / denom;

    return std::isfinite(solidAngle) && solidAngle > 0.0f
        ? solidAngle
        : 0.0f;
}

void RtEnvironmentImportance::BuildAliasTable(
    const std::vector<float>& probabilities,
    const std::vector<float>& solidAngle,
    std::vector<RtEnvAliasEntry>& out)
{
    const size_t n = probabilities.size();

    out.clear();

    if (n == 0 || solidAngle.size() != n)
        return;

    out.assign(n, RtEnvAliasEntry{});

    double sum = 0.0;

    for (float p : probabilities)
    {
        if (std::isfinite(p) && p > 0.0f)
            sum += static_cast<double>(p);
    }

    if (!std::isfinite(sum) || sum <= 0.0)
        return;

    std::vector<double> scaled(n, 0.0);
    std::vector<uint32_t> small;
    std::vector<uint32_t> large;

    small.reserve(n);
    large.reserve(n);

    for (size_t i = 0; i < n; ++i)
    {
        const double p =
            (std::isfinite(probabilities[i]) && probabilities[i] > 0.0f)
            ? static_cast<double>(probabilities[i]) / sum
            : 0.0;

        const float sa = solidAngle[i];

        out[i].alias = static_cast<uint32_t>(i);
        out[i].q = 1.0f;
        out[i].pdf =
            (std::isfinite(sa) && sa > 0.0f)
            ? static_cast<float>(p / static_cast<double>(sa))
            : 0.0f;

        scaled[i] = p * static_cast<double>(n);

        if (scaled[i] < 1.0)
            small.push_back(static_cast<uint32_t>(i));
        else
            large.push_back(static_cast<uint32_t>(i));
    }

    while (!small.empty() && !large.empty())
    {
        const uint32_t s = small.back();
        small.pop_back();

        const uint32_t l = large.back();
        large.pop_back();

        out[s].q = std::clamp(static_cast<float>(scaled[s]), 0.0f, 1.0f);
        out[s].alias = l;

        scaled[l] = (scaled[l] + scaled[s]) - 1.0;

        if (scaled[l] < 1.0)
            small.push_back(l);
        else
            large.push_back(l);
    }

    for (uint32_t i : large)
    {
        out[i].q = 1.0f;
        out[i].alias = i;
    }

    for (uint32_t i : small)
    {
        out[i].q = 1.0f;
        out[i].alias = i;
    }
}

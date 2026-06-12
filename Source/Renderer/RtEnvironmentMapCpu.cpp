#include "RtEnvironmentMapCpu.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    bool CheckedCubeRadianceCount(uint32_t faceSize, size_t& outCount)
    {
        outCount = 0;

        if (faceSize == 0)
            return false;

        const uint64_t fs = static_cast<uint64_t>(faceSize);
        const uint64_t count64 = 6ull * fs * fs;

        if (count64 > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
            return false;

        outCount = static_cast<size_t>(count64);
        return true;
    }

    float SanitizeRadiance(float v)
    {
        if (!std::isfinite(v) || v <= 0.0f)
            return 0.0f;

        return v;
    }

    DirectX::XMFLOAT3 DirectionFromCubeFaceUV(uint32_t face, float u01, float v01)
    {
        const float u = u01 * 2.0f - 1.0f;
        const float v = v01 * 2.0f - 1.0f;

        DirectX::XMFLOAT3 d{};

        if (face == 0u)
            d = DirectX::XMFLOAT3(1.0f, -v, -u);
        else if (face == 1u)
            d = DirectX::XMFLOAT3(-1.0f, -v, u);
        else if (face == 2u)
            d = DirectX::XMFLOAT3(u, 1.0f, v);
        else if (face == 3u)
            d = DirectX::XMFLOAT3(u, -1.0f, -v);
        else if (face == 4u)
            d = DirectX::XMFLOAT3(u, -v, 1.0f);
        else
            d = DirectX::XMFLOAT3(-u, -v, -1.0f);

        const DirectX::XMVECTOR n =
            DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&d));

        DirectX::XMStoreFloat3(&d, n);
        return d;
    }

    DirectX::XMFLOAT2 LatLongUVFromDirection(const DirectX::XMFLOAT3& dir)
    {
        const float u =
            std::atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;

        const float v =
            std::asin(std::clamp(dir.y, -1.0f, 1.0f)) / kPi + 0.5f;

        return DirectX::XMFLOAT2(u, 1.0f - v);
    }

    DirectX::XMFLOAT4 ReadFloat4Pixel(
        const DirectX::Image& image,
        uint32_t x,
        uint32_t y)
    {
        const auto* row =
            reinterpret_cast<const DirectX::XMFLOAT4*>(
                image.pixels + static_cast<size_t>(y) * image.rowPitch);

        return row[x];
    }

    DirectX::XMFLOAT3 SampleLatLongBilinear(
        const DirectX::Image& image,
        float u,
        float v)
    {
        const uint32_t width = static_cast<uint32_t>(image.width);
        const uint32_t height = static_cast<uint32_t>(image.height);

        if (width == 0 || height == 0)
            return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

        // Match shader behavior: horizontal wrap, vertical clamp.
        u = u - std::floor(u);
        v = std::clamp(v, 0.0f, 1.0f);

        const float fx = u * static_cast<float>(width) - 0.5f;
        const float fy = v * static_cast<float>(height) - 0.5f;

        const int x0i = static_cast<int>(std::floor(fx));
        const int y0i = static_cast<int>(std::floor(fy));

        const float tx = fx - static_cast<float>(x0i);
        const float ty = fy - static_cast<float>(y0i);

        const int widthI = static_cast<int>(width);
        const int heightI = static_cast<int>(height);

        const int x0 =
            ((x0i % widthI) + widthI) % widthI;

        const int x1 =
            ((x0i + 1) % widthI + widthI) % widthI;

        const int y0 =
            std::clamp(y0i, 0, heightI - 1);

        const int y1 =
            std::clamp(y0i + 1, 0, heightI - 1);

        const DirectX::XMFLOAT4 c00 =
            ReadFloat4Pixel(image, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0));

        const DirectX::XMFLOAT4 c10 =
            ReadFloat4Pixel(image, static_cast<uint32_t>(x1), static_cast<uint32_t>(y0));

        const DirectX::XMFLOAT4 c01 =
            ReadFloat4Pixel(image, static_cast<uint32_t>(x0), static_cast<uint32_t>(y1));

        const DirectX::XMFLOAT4 c11 =
            ReadFloat4Pixel(image, static_cast<uint32_t>(x1), static_cast<uint32_t>(y1));

        const float wx0 = 1.0f - tx;
        const float wy0 = 1.0f - ty;

        const float r =
            (c00.x * wx0 + c10.x * tx) * wy0 +
            (c01.x * wx0 + c11.x * tx) * ty;

        const float g =
            (c00.y * wx0 + c10.y * tx) * wy0 +
            (c01.y * wx0 + c11.y * tx) * ty;

        const float b =
            (c00.z * wx0 + c10.z * tx) * wy0 +
            (c01.z * wx0 + c11.z * tx) * ty;

        return DirectX::XMFLOAT3(
            SanitizeRadiance(r),
            SanitizeRadiance(g),
            SanitizeRadiance(b));
    }

    bool LoadImageAsRGBA32F(
        const std::filesystem::path& path,
        DirectX::ScratchImage& outImage)
    {
        using namespace DirectX;

        outImage.Release();

        ScratchImage source;
        TexMetadata metadata{};

        const std::wstring wpath = path.wstring();
        const std::wstring ext = path.extension().wstring();

        HRESULT hr = S_OK;

        if (ext == L".dds" || ext == L".DDS")
        {
            hr = LoadFromDDSFile(
                wpath.c_str(),
                DDS_FLAGS_NONE,
                &metadata,
                source);
        }
        else if (ext == L".hdr" || ext == L".HDR")
        {
            hr = LoadFromHDRFile(
                wpath.c_str(),
                &metadata,
                source);
        }
        else
        {
            hr = LoadFromWICFile(
                wpath.c_str(),
                WIC_FLAGS_NONE,
                &metadata,
                source);
        }

        if (FAILED(hr))
            return false;

        if (IsCompressed(metadata.format))
        {
            ScratchImage decompressed;

            hr = Decompress(
                source.GetImages(),
                source.GetImageCount(),
                metadata,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                decompressed);

            if (FAILED(hr))
                return false;

            source = std::move(decompressed);
            metadata = source.GetMetadata();
        }

        ScratchImage converted;

        hr = Convert(
            source.GetImages(),
            source.GetImageCount(),
            metadata,
            DXGI_FORMAT_R32G32B32A32_FLOAT,
            TEX_FILTER_DEFAULT,
            TEX_THRESHOLD_DEFAULT,
            converted);

        if (FAILED(hr))
            return false;

        outImage = std::move(converted);
        return true;
    }
}

bool LoadLatLongEnvironmentAsCubeFaces(
    const std::filesystem::path& path,
    uint32_t faceSize,
    std::vector<DirectX::XMFLOAT3>& outRadiance)
{
    outRadiance.clear();

    size_t count = 0;
    if (!CheckedCubeRadianceCount(faceSize, count))
        return false;

    DirectX::ScratchImage image;
    if (!LoadImageAsRGBA32F(path, image))
        return false;

    const DirectX::Image* src = image.GetImage(0, 0, 0);
    if (!src || !src->pixels || src->width == 0 || src->height == 0)
        return false;

    std::vector<DirectX::XMFLOAT3> radiance(count);

    const uint32_t faceTexelCount = faceSize * faceSize;

    for (uint32_t face = 0; face < 6u; ++face)
    {
        for (uint32_t y = 0; y < faceSize; ++y)
        {
            for (uint32_t x = 0; x < faceSize; ++x)
            {
                const float cubeU =
                    (static_cast<float>(x) + 0.5f) /
                    static_cast<float>(faceSize);

                const float cubeV =
                    (static_cast<float>(y) + 0.5f) /
                    static_cast<float>(faceSize);

                const DirectX::XMFLOAT3 dir =
                    DirectionFromCubeFaceUV(face, cubeU, cubeV);

                const DirectX::XMFLOAT2 uv =
                    LatLongUVFromDirection(dir);

                const size_t index =
                    static_cast<size_t>(face) * faceTexelCount +
                    static_cast<size_t>(y) * faceSize +
                    x;

                radiance[index] =
                    SampleLatLongBilinear(*src, uv.x, uv.y);
            }
        }
    }

    outRadiance = std::move(radiance);
    return true;
}

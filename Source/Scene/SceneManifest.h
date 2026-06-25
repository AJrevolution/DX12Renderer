#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SceneModelDesc
{
    std::string name;
    std::filesystem::path path;

    bool enabled = true;
    bool rasterEnabled = true;
    bool dxrEnabled = true;
    bool castShadows = true;

    DirectX::XMFLOAT3 translation = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 rotationDegrees = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };

    uint32_t objectIdBase = 0;

    DirectX::XMMATRIX BuildTransformMatrix() const;
};

struct SceneSunDesc
{
    bool enabled = false;
    DirectX::XMFLOAT3 direction = { -0.35f, -1.0f, -0.25f };
    DirectX::XMFLOAT3 color = { 1.0f, 0.95f, 0.88f };
    float intensity = 3.0f;
};

struct ScenePointLightDesc
{
    std::string name;
    bool enabled = true;

    DirectX::XMFLOAT3 position = { 0.0f, 2.0f, 0.0f };
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };

    float intensity = 10.0f;
    float range = 5.0f;
};

struct SceneManifest
{
    std::string name;

    std::vector<SceneModelDesc> models;
    std::vector<ScenePointLightDesc> pointLights;

    SceneSunDesc sun;
    bool hasSun = false;

    std::wstring lastError;

    bool LoadFromFile(const std::filesystem::path& path);
};

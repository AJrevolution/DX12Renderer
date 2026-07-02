#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class SceneProceduralGeometryMode : uint8_t
{
    Auto,
    Always,
    Never
};

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


struct SceneEnvironmentDesc
{
    bool enabled = false;

    std::filesystem::path displayPath;
    std::filesystem::path lightingPath;

    //explicit precomputed lighting assets.
    std::filesystem::path lightingDiffusePath;
    std::filesystem::path lightingSpecularPath;
    std::filesystem::path lightingRadiancePath;

    bool useDisplayForLighting = false;

    float displayIntensity = 1.0f;
    float lightingIntensity = 1.0f;
    float rotationDegrees = 0.0f;

    // Optional lighting rotation. Defaults to rotationDegrees while parsing.
    float lightingRotationDegrees = 0.0f;

    bool visibleInRaster = true;
    bool visibleInDxr = true;
    bool specularMissUsesDisplaySky = false;

    DirectX::XMFLOAT3 fallbackTopColor = { 0.20f, 0.36f, 0.62f };
    DirectX::XMFLOAT3 fallbackHorizonColor = { 0.76f, 0.80f, 0.86f };
    DirectX::XMFLOAT3 fallbackBottomColor = { 0.42f, 0.45f, 0.50f };
};

struct SceneProceduralGeometryDesc
{
    SceneProceduralGeometryMode mode =
        SceneProceduralGeometryMode::Auto;
};

struct SceneCameraDesc
{
    bool enabled = true;

    DirectX::XMFLOAT3 position = { 6.0f, 3.0f, -8.0f };
    DirectX::XMFLOAT3 target = { 0.0f, 1.0f, 0.0f };

    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

struct SceneManifest
{
    std::string name;

    std::vector<SceneModelDesc> models;
    std::vector<ScenePointLightDesc> pointLights;

    SceneSunDesc sun;
    bool hasSun = false;

    std::wstring lastError;

    SceneEnvironmentDesc environment;
    bool hasEnvironment = false;

    bool LoadFromFile(const std::filesystem::path& path);

    SceneProceduralGeometryDesc proceduralGeometry;
    bool hasProceduralGeometry = false;

    SceneCameraDesc camera;
    bool hasCamera = false;
};

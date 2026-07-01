#include "SceneManifest.h"

#include "ThirdParty/tinygltf/json.hpp"

#include <DirectXMath.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <exception>

using json = nlohmann::json;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    std::wstring ToWide(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }

    bool ReadFileToString(
        const std::filesystem::path& path,
        std::string& out)
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
            return false;

        std::ostringstream ss;
        ss << file.rdbuf();
        out = ss.str();
        return true;
    }

    bool JsonBool(
        const json& j,
        const char* name,
        bool fallback)
    {
        if (!j.contains(name) || !j[name].is_boolean())
            return fallback;

        return j[name].get<bool>();
    }

    float JsonFloat(
        const json& j,
        const char* name,
        float fallback)
    {
        if (!j.contains(name) || !j[name].is_number())
            return fallback;

        return j[name].get<float>();
    }

    uint32_t JsonUint(
        const json& j,
        const char* name,
        uint32_t fallback)
    {
        if (!j.contains(name) || !j[name].is_number_unsigned())
            return fallback;

        return j[name].get<uint32_t>();
    }

    std::string JsonString(
        const json& j,
        const char* name,
        const std::string& fallback = {})
    {
        if (!j.contains(name) || !j[name].is_string())
            return fallback;

        return j[name].get<std::string>();
    }

    DirectX::XMFLOAT3 JsonFloat3(
        const json& j,
        const char* name,
        DirectX::XMFLOAT3 fallback)
    {
        if (!j.contains(name) ||
            !j[name].is_array() ||
            j[name].size() < 3)
        {
            return fallback;
        }

        const json& a = j[name];

        if (!a[0].is_number() ||
            !a[1].is_number() ||
            !a[2].is_number())
        {
            return fallback;
        }

        return
        {
            a[0].get<float>(),
            a[1].get<float>(),
            a[2].get<float>()
        };
    }

    DirectX::XMFLOAT3 NormaliseOrFallback(
        DirectX::XMFLOAT3 v,
        DirectX::XMFLOAT3 fallback)
    {
        using namespace DirectX;

        XMVECTOR n =
            XMVectorSet(v.x, v.y, v.z, 0.0f);

        const float lenSq =
            XMVectorGetX(XMVector3LengthSq(n));

        if (lenSq <= 1e-8f)
            return fallback;

        n = XMVector3Normalize(n);

        XMFLOAT3 out{};
        XMStoreFloat3(&out, n);
        return out;
    }

    SceneProceduralGeometryMode ParseProceduralGeometryMode(
        const std::string& value)
    {
        if (value == "auto")
            return SceneProceduralGeometryMode::Auto;

        if (value == "always")
            return SceneProceduralGeometryMode::Always;

        if (value == "never")
            return SceneProceduralGeometryMode::Never;

        return SceneProceduralGeometryMode::Auto;
    }
}

DirectX::XMMATRIX SceneModelDesc::BuildTransformMatrix() const
{
    using namespace DirectX;

    const float pitch =
        rotationDegrees.x * kPi / 180.0f;

    const float yaw =
        rotationDegrees.y * kPi / 180.0f;

    const float roll =
        rotationDegrees.z * kPi / 180.0f;

    return
        XMMatrixScaling(scale.x, scale.y, scale.z) *
        XMMatrixRotationRollPitchYaw(pitch, yaw, roll) *
        XMMatrixTranslation(
            translation.x,
            translation.y,
            translation.z);
}

bool SceneManifest::LoadFromFile(const std::filesystem::path& path)
{
    name.clear();
    models.clear();
    pointLights.clear();
    hasSun = false;
    sun = {};
    environment = {};
    hasEnvironment = false;
    lastError.clear();
    proceduralGeometry = {};
    hasProceduralGeometry = false;

    std::string text;

    if (!ReadFileToString(path, text))
    {
        lastError =
            L"SceneManifest::LoadFromFile failed to open file: " +
            path.wstring();

        return false;
    }

    json root;

    try
    {
        root = json::parse(text);
    }
    catch (const std::exception& e)
    {
        lastError =
            L"SceneManifest::LoadFromFile JSON parse failed: " +
            ToWide(e.what());

        return false;
    }

    if (!root.is_object())
    {
        lastError = L"SceneManifest root must be a JSON object.";
        return false;
    }

    name = JsonString(root, "name", "scene");

    if (root.contains("proceduralGeometry") &&
        root["proceduralGeometry"].is_object())
    {
        const json& p = root["proceduralGeometry"];

        hasProceduralGeometry = true;

        proceduralGeometry.mode =
            ParseProceduralGeometryMode(
                JsonString(p, "mode", "auto"));
    }

    if (root.contains("models") && root["models"].is_array())
    {
        const json& modelArray = root["models"];

        for (size_t i = 0; i < modelArray.size(); ++i)
        {
            const json& entry = modelArray[i];

            if (!entry.is_object())
                continue;

            SceneModelDesc model{};

            model.name =
                JsonString(
                    entry,
                    "name",
                    "model_" + std::to_string(i));

            const std::string pathString =
                JsonString(entry, "path");

            if (pathString.empty())
                continue;

            model.path = std::filesystem::path(pathString);

            model.enabled =
                JsonBool(entry, "enabled", model.enabled);

            model.rasterEnabled =
                JsonBool(entry, "rasterEnabled", model.rasterEnabled);

            model.dxrEnabled =
                JsonBool(entry, "dxrEnabled", model.dxrEnabled);

            model.castShadows =
                JsonBool(entry, "castShadows", model.castShadows);

            model.translation =
                JsonFloat3(entry, "translation", model.translation);

            model.rotationDegrees =
                JsonFloat3(entry, "rotationDegrees", model.rotationDegrees);

            model.scale =
                JsonFloat3(entry, "scale", model.scale);

            model.objectIdBase =
                JsonUint(entry, "objectIdBase", model.objectIdBase);

            models.push_back(model);
        }
    }

    if (root.contains("sun") && root["sun"].is_object())
    {
        const json& s = root["sun"];

        hasSun = true;

        sun.enabled =
            JsonBool(s, "enabled", true);

        sun.direction =
            NormaliseOrFallback(
                JsonFloat3(s, "direction", sun.direction),
                sun.direction);

        sun.color =
            JsonFloat3(s, "color", sun.color);

        sun.intensity =
            JsonFloat(s, "intensity", sun.intensity);
    }

    if (root.contains("pointLights") && root["pointLights"].is_array())
    {
        const json& lights = root["pointLights"];

        for (size_t i = 0; i < lights.size(); ++i)
        {
            const json& entry = lights[i];

            if (!entry.is_object())
                continue;

            ScenePointLightDesc light{};

            light.name =
                JsonString(
                    entry,
                    "name",
                    "point_light_" + std::to_string(i));

            light.enabled =
                JsonBool(entry, "enabled", light.enabled);

            light.position =
                JsonFloat3(entry, "position", light.position);

            light.color =
                JsonFloat3(entry, "color", light.color);

            light.intensity =
                JsonFloat(entry, "intensity", light.intensity);

            light.range =
                JsonFloat(entry, "range", light.range);

            if (light.enabled)
                pointLights.push_back(light);
        }
    }

    if (root.contains("environment") &&
        root["environment"].is_object())
    {
        const json& e = root["environment"];

        hasEnvironment = true;
        environment = {};

        environment.enabled =
            JsonBool(e, "enabled", true);

        const std::string displayPath =
            JsonString(e, "displayPath");

        const std::string lightingPath =
            JsonString(e, "lightingPath");

        const std::string lightingDiffusePath =
            JsonString(e, "lightingDiffusePath");

        const std::string lightingSpecularPath =
            JsonString(e, "lightingSpecularPath");

        const std::string lightingRadiancePath =
            JsonString(e, "lightingRadiancePath");

        environment.displayPath =
            std::filesystem::path(displayPath);

        environment.lightingPath =
            std::filesystem::path(lightingPath);

        environment.lightingDiffusePath =
            std::filesystem::path(lightingDiffusePath);

        environment.lightingSpecularPath =
            std::filesystem::path(lightingSpecularPath);

        environment.lightingRadiancePath =
            std::filesystem::path(lightingRadiancePath);

        environment.useDisplayForLighting =
            JsonBool(
                e,
                "useDisplayForLighting",
                environment.useDisplayForLighting);

        environment.displayIntensity =
            std::max(
                0.0f,
                JsonFloat(
                    e,
                    "displayIntensity",
                    environment.displayIntensity));

        environment.lightingIntensity =
            std::max(
                0.0f,
                JsonFloat(
                    e,
                    "lightingIntensity",
                    environment.lightingIntensity));

        environment.rotationDegrees =
            JsonFloat(
                e,
                "rotationDegrees",
                environment.rotationDegrees);

        environment.lightingRotationDegrees =
            JsonFloat(
                e,
                "lightingRotationDegrees",
                environment.rotationDegrees);

        environment.visibleInRaster =
            JsonBool(
                e,
                "visibleInRaster",
                environment.visibleInRaster);

        environment.visibleInDxr =
            JsonBool(
                e,
                "visibleInDxr",
                environment.visibleInDxr);

        environment.specularMissUsesDisplaySky =
            JsonBool(
                e,
                "specularMissUsesDisplaySky",
                environment.specularMissUsesDisplaySky);

        environment.fallbackTopColor =
            JsonFloat3(
                e,
                "fallbackTopColor",
                environment.fallbackTopColor);

        environment.fallbackHorizonColor =
            JsonFloat3(
                e,
                "fallbackHorizonColor",
                environment.fallbackHorizonColor);

        environment.fallbackBottomColor =
            JsonFloat3(
                e,
                "fallbackBottomColor",
                environment.fallbackBottomColor);
    }

    return true;
}

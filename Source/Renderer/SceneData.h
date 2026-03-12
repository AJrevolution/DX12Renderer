#pragma once
#include <DirectXMath.h>

struct DirectionalLight
{
    DirectX::XMFLOAT3 direction = { 0.577f, -0.577f, 0.577f };
    float intensity = 1.0f;

    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
    float pad = 0.0f;
};

struct CameraSettings
{
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, -2.0f };
    float fovY = DirectX::XM_PIDIV4;

    DirectX::XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };
    float nearZ = 0.1f;

    float farZ = 100.0f;
    float pad[3] = {};
};

struct SceneData
{
    CameraSettings camera;
    DirectionalLight sun;
};

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <DirectXMath.h>

// Loads a latlong/equirectangular environment image on the CPU,
// resamples it into cubemap face-major radiance, and returns:
//   face 0 texels, face 1 texels, ... face 5 texels
//
// Output size must be:
//   6 * faceSize * faceSize
//
// The cubemap face direction convention intentionally matches
// CubeFaceUVToDirection() in RtSampling.hlsli.
bool LoadLatLongEnvironmentAsCubeFaces(
    const std::filesystem::path& path,
    uint32_t faceSize,
    std::vector<DirectX::XMFLOAT3>& outRadiance);

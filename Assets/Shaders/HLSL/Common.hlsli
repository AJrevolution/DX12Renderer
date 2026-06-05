#ifndef ASSETS_SHADERS_HLSL_COMMON_HLSLI
#define ASSETS_SHADERS_HLSL_COMMON_HLSLI


// Conventions:
// - BaseColor textures sampled as sRGB (SRV format _SRGB) -> hardware converts to linear.
// - Lighting/math in linear.
//   TODO: real tonemap + output transform.

// -----------------------------------------------------------------------------
// Distance / ViewZ measurement contract
//
// Depth01:
//   Existing normalized screen/depth-buffer style guide.
//   Used as legacy diagnostic / sky discriminator.
//   Sky/miss convention remains depth >= 0.9999.
//
// ViewZRaw / ViewZRecons:
//   Linear primary visible-surface distance in world units.
//   Current implementation stores RayT along the primary camera ray.
//   Treat this as the project's ViewZ-compatible primary distance guide,
//   not as strict camera-space Z.
//   This is not specular secondary-ray hit distance.
//   Invalid convention: -1.0.
//
// ViewZReconsConf:
//   Confidence for reconstructed ViewZ.
//   Invalid / unknown convention: 0.0.
//
// Normalized distance:
//   Converts a linear distance into a stable [0,1] comparison space.
//   Callers must use the same params consistently across passes.
//   params.x = relative scale against reference viewZ, default 1.
//   params.y = absolute bias in world units, default 0.
//   params.z = roughness scale contribution, default 1.
// -----------------------------------------------------------------------------


static const float PI = 3.14159265359f;
static const float DISTANCE_INVALID = -1.0f;

bool DistanceValid(float d)
{
    return d >= 0.0f;
}

float NormalizeDistance(float dist, float viewZ, float roughness, float3 params)
{
    if (!DistanceValid(dist))
        return 0.0f;

    float relativeScale = max(params.x, 1e-4f);
    float absoluteBias = max(params.y, 0.0f);
    float roughScale = lerp(1.0f, max(params.z, 1e-4f), saturate(roughness));

    float denom =
        max(1e-3f, abs(viewZ) * relativeScale * roughScale + absoluteBias);

    // Bounded, monotonic, and stable under scene-scale changes when viewZ is
    // the local reference distance.
    return saturate(dist / (dist + denom));
}

float DistanceSimilarityWeight(float norm0, float norm1, float sigma)
{
    return exp(-abs(norm0 - norm1) / max(1e-4f, sigma));
}

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    return (len2 > 1e-8f) ? v * rsqrt(len2) : float3(0, 0, 1);
}

float3 SRGBToLinear(float3 c)
{
    // Only use if you sample non-sRGB resources. Prefer SRGB SRVs for baseColor.
    return pow(c, 2.2f);
}

float3 LinearToSRGB(float3 c)
{
    return pow(saturate(c), 1.0f / 2.2f);
}
#endif // ASSETS_SHADERS_HLSL_COMMON
#ifndef ASSETS_SHADERS_HLSL_COMMON_HLSLI
#define ASSETS_SHADERS_HLSL_COMMON_HLSLI


// Conventions:
// - BaseColor textures sampled as sRGB (SRV format _SRGB) -> hardware converts to linear.
// - Lighting/math in linear.
//   TODO: real tonemap + output transform.

static const float PI = 3.14159265359f;

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
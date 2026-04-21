#include "Common.hlsli"

Texture2D<float4> g_Accum : register(t0);
Texture2D<float4> g_AovNormal : register(t1);
Texture2D<float> g_AovDepth : register(t2);

RWTexture2D<float4> g_Output : register(u0);

cbuffer DenoiseConstants : register(b0)
{
    float2 InvResolution;
    int Radius;
    float SigmaDepth;
    float SigmaNormal;
    float NormalPower;
    float2 _pad;
};

float SpatialWeight(int2 d)
{
    float dist2 = float(d.x * d.x + d.y * d.y);
    float sigmaSpatial = max(1.0f, float(Radius));
    return exp(-dist2 / max(1e-4f, 2.0f * sigmaSpatial * sigmaSpatial));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;
    uint width, height;
    g_Output.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 centerColor = g_Accum[pixel].rgb;
    float3 n0 = g_AovNormal[pixel].xyz * 2.0f - 1.0f;
    float z0 = g_AovDepth[pixel];

    // Sky / far plane fallback.
    if (z0 >= 0.9999f)
    {
        g_Output[pixel] = float4(LinearToSRGB(centerColor), 1.0f);
        return;
    }

    float3 sum = 0.0f.xxx;
    float wsum = 0.0f;

    for (int y = -Radius; y <= Radius; ++y)
    {
        for (int x = -Radius; x <= Radius; ++x)
        {
            int2 p = int2(pixel) + int2(x, y);
            p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float3 c = g_Accum[p].rgb;
            float3 n1 = g_AovNormal[p].xyz * 2.0f - 1.0f;
            float z1 = g_AovDepth[p];

            float ws = SpatialWeight(int2(x, y));
            float wn = pow(saturate(dot(normalize(n0), normalize(n1))), NormalPower);
            float wz = exp(-abs(z0 - z1) / max(1e-4f, SigmaDepth));

            float w = ws * wn * wz;
            sum += c * w;
            wsum += w;
        }
    }

    float3 filtered = (wsum > 1e-6f) ? (sum / wsum) : centerColor;
    g_Output[pixel] = float4(LinearToSRGB(filtered), 1.0f);
}
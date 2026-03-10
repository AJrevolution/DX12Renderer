#ifndef ASSETS_SHADERS_HLSL_PBR_HLSLI
#define ASSETS_SHADERS_HLSL_PBR_HLSLI
#include "Common.hlsli"

// Fresnel Schlick
float3 F_Schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * Pow5(1.0f - cosTheta);
}

// GGX / Trowbridge-Reitz normal distribution function
float D_GGX(float NdotH, float alpha)
{
    float a2 = alpha * alpha;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-7f);
}

// Smith geometry term (Schlick-GGX)
float G_SchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-7f);
}

float G_Smith(float NdotV, float NdotL, float roughness)
{
    // UE4-style k
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return G_SchlickGGX(NdotV, k) * G_SchlickGGX(NdotL, k);
}

struct PbrInputs
{
    float3 N;
    float3 V;
    float3 L;
    float3 albedo; // linear
    float metallic;
    float roughness;
};

float3 EvalDirectPBR(PbrInputs i, float3 lightColor)
{
    float3 N = SafeNormalize(i.N);
    float3 V = SafeNormalize(i.V);
    float3 L = SafeNormalize(i.L);
    float3 H = SafeNormalize(V + L);

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float rough = saturate(i.roughness);
    float alpha = max(rough * rough, 0.002f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), i.albedo, saturate(i.metallic));

    float3 F = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, alpha);
    float G = G_Smith(NdotV, NdotL, rough);

    float3 spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-6f);

    float3 kd = (1.0f - F) * (1.0f - saturate(i.metallic));
    float3 diff = kd * i.albedo / PI;

    return (diff + spec) * lightColor * NdotL;
}
#endif
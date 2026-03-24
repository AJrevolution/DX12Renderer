#ifndef DEBUG_VIEW
#define DEBUG_VIEW 0
#endif

#include "Common.hlsli"
#include "PBR.hlsli"

// Scene-wide table (space0)
Texture2D g_BRDFLut : register(t0, space0);
Texture2D g_IBLDiffuse : register(t1, space0);
Texture2D g_IBLSpecular : register(t2, space0);
Texture2D g_ShadowMap : register(t3, space0); // reserved

Texture2D g_GBuffer0 : register(t0, space1); // baseColor
Texture2D g_GBuffer1 : register(t1, space1); // normal
Texture2D g_GBuffer2 : register(t2, space1); // M/R/Ao
Texture2D g_Depth    : register(t3, space1); // depth SRV (R32_FLOAT)

SamplerState g_LinearClamp : register(s1);

cbuffer PerFrameConstants : register(b0)
{
    row_major float4x4 ViewProj;
    row_major float4x4 InvViewProj;
    
    float3 CameraPos;
    float Time;
    
    uint FrameIndex;
    uint HasBRDFLut;
    uint HasIBL;
    uint _pad0;

    float3 LightDir;
    float pad1;
    float3 LightColor;
    float pad2;
};

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = (1.0f - uv.y) * 2.0f - 1.0f;

    float4 clip = float4(ndc, depth, 1.0f);
    float4 world = mul(clip, InvViewProj);
    return world.xyz / world.w;
}

float2 DirToLatLongUV(float3 d)
{
    d = normalize(d);
    float u = atan2(d.z, d.x) / (2.0f * PI) + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / PI + 0.5f;
    return float2(u, 1.0f - v);
}

float4 main(PSIn i) : SV_Target
{
    float3 base = g_GBuffer0.Sample(g_LinearClamp, i.uv).rgb;
    float3 N = normalize(g_GBuffer1.Sample(g_LinearClamp, i.uv).xyz);

    float4 mrao = g_GBuffer2.Sample(g_LinearClamp, i.uv);
    float metallic = saturate(mrao.x);
    float roughness = saturate(mrao.y);
    float ao = saturate(mrao.z);
    
    float depth = g_Depth.Sample(g_LinearClamp, i.uv).r;
    float3 worldPos = ReconstructWorldPos(i.uv, depth);
    
    float3 V = SafeNormalize(CameraPos - worldPos);
    float3 L = SafeNormalize(-LightDir);
    
    PbrInputs p;
    p.N = N;
    p.V = V;
    p.L = L;
    p.albedo = base;
    p.metallic = metallic;
    p.roughness = roughness;

    float3 direct = EvalDirectPBR(p, LightColor);

    // Deferred IBL using the same scene contract as forward
    float NdotV = saturate(dot(N, V));
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);

    float2 brdf;
    if (HasBRDFLut > 0)
        brdf = g_BRDFLut.Sample(g_LinearClamp, float2(NdotV, roughness)).rg;
    else
        brdf = float2(1.0f, 0.0f);

    float3 diffuseEnv;
    float3 specularEnv;

    if (HasIBL > 0)
    {
        float2 diffuseUV = DirToLatLongUV(N);
        diffuseEnv = g_IBLDiffuse.Sample(g_LinearClamp, diffuseUV).rgb;

        float3 R = reflect(-V, N);
        float3 Rrough = normalize(lerp(R, N, roughness * roughness));

        float2 specUV0 = DirToLatLongUV(R);
        float2 specUV1 = DirToLatLongUV(Rrough);

        float3 specSharp = g_IBLSpecular.Sample(g_LinearClamp, specUV0).rgb;
        float3 specBlur = g_IBLSpecular.Sample(g_LinearClamp, specUV1).rgb;
        specularEnv = lerp(specSharp, specBlur, roughness);
    }
    else
    {
        diffuseEnv = float3(0.03f, 0.03f, 0.03f);
        specularEnv = float3(0.0f, 0.0f, 0.0f);
    }

    float3 F = F_Schlick(NdotV, F0);
    float3 kd = (1.0f - F) * (1.0f - metallic);

    float3 iblDiffuse = kd * base * diffuseEnv * ao;
    float3 iblSpec = specularEnv * (F0 * brdf.x + brdf.y);

    float3 lit = direct + iblDiffuse + iblSpec;
    
    #if DEBUG_VIEW == 1
        return float4(N * 0.5f + 0.5f, 1.0f);
    #elif DEBUG_VIEW == 2
        return float4(roughness.xxx, 1.0f);
    #elif DEBUG_VIEW == 3
        return float4(metallic.xxx, 1.0f);
    #elif DEBUG_VIEW == 4
        return float4(depth.xxx, 1.0f);
    #endif
    
    return float4(LinearToSRGB(lit), 1.0f);
}

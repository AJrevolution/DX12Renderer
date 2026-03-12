#include "Common.hlsli"
#include "PBR.hlsli"

// IBL diffuse/specular slots are reserved but currently use constant fallback env colors.
// This keeps the binding model stable until real env assets are added.

Texture2D g_BRDFLut : register(t0, space0);
Texture2D g_IBLDiffuse : register(t1, space0); // placeholder
Texture2D g_IBLSpecular : register(t2, space0); // placeholder
Texture2D g_ShadowMap : register(t3, space0); // placeholder

Texture2D g_BaseColor : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MetalRough : register(t2, space1); 

SamplerState g_LinearWrap : register(s0);
SamplerState g_LinearClamp : register(s1);
SamplerState g_AnisoWrap : register(s2);
SamplerState g_PointClamp : register(s3);

cbuffer PerFrameConstants : register(b0)
{
    row_major float4x4 ViewProj;
    float3 CameraPos;
    float Time;
    uint FrameIndex;
    float3 _pad0;

    float3 LightDir;
    float _pad1;
    float3 LightColor;
    float _pad2;
};

cbuffer PerDrawConstants : register(b1)
{
    row_major float4x4 World;
    uint MaterialIndex;
    uint3 _padA;

    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float2 _padB;
};

struct PSIn
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldN : TEXCOORD1;
    float4 worldT : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 col : COLOR;
};

float3 DecodeNormal(float3 n)
{
    return normalize(n * 2.0f - 1.0f);
}

float4 main(PSIn i) : SV_Target
{
    float3 base = g_BaseColor.Sample(g_LinearWrap, i.uv).rgb; // if SRV is _SRGB, this is already linear
    base *= BaseColorFactor.rgb;
    base *= i.col.rgb;
    
    float3 tangentNormal = DecodeNormal(g_NormalMap.Sample(g_LinearWrap, i.uv).xyz);

    float3 N = normalize(i.worldN);
    float3 T = normalize(i.worldT.xyz);
    float3 B = normalize(cross(N, T)) * i.worldT.w;
    
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(tangentNormal, TBN));
    
    float2 mr = g_MetalRough.Sample(g_LinearWrap, i.uv).gb;
    float roughness = saturate(mr.x * RoughnessFactor); // G
    float metallic = saturate(mr.y * MetallicFactor); // B
    
    float3 V = SafeNormalize(CameraPos - i.worldPos);
    float3 L = SafeNormalize(-LightDir);
    
    PbrInputs p;
    p.N = worldNormal;
    p.V = V;
    p.L = L;
    p.albedo = base;
    p.metallic = metallic;
    p.roughness = roughness;
     
    // Direct light
    float3 direct = EvalDirectPBR(p, LightColor);

    // Minimal IBL approximation using BRDF LUT + constant env color
    float NdotV = saturate(dot(worldNormal, V));
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    
    float2 brdf = g_BRDFLut.Sample(g_LinearClamp, float2(NdotV, roughness)).rg;

    // Placeholder environment colors until cubemaps are added
    float3 diffuseEnvColor = float3(0.03f, 0.04f, 0.05f);
    float3 specularEnvColor = float3(0.04f, 0.04f, 0.04f);
    
    float3 F = F_Schlick(NdotV, F0);
    float3 kd = (1.0f - F) * (1.0f - metallic);

    float3 iblDiffuse = kd * base * diffuseEnvColor;
    float3 iblSpec = specularEnvColor * (F0 * brdf.x + brdf.y);
    
    float3 lit = direct + iblDiffuse + iblSpec;

    // Temporary output transform since swapchain is UNORM (not SRGB)
    float3 outColor = LinearToSRGB(lit);

    return float4(outColor, 1.0f);

}

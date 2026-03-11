#include "Common.hlsli"
#include "PBR.hlsli"

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
    
    PbrInputs p;
    p.N = worldNormal;
    p.V = SafeNormalize(CameraPos - i.worldPos);
    p.L = SafeNormalize(-LightDir);
    p.albedo = base;
    p.metallic = metallic;
    p.roughness = roughness;

    float3 lit = EvalDirectPBR(p, LightColor);

    // Temporary output transform since swapchain is UNORM (not SRGB)
    float3 outColor = LinearToSRGB(lit);
    //return float4(i.worldT.www, 1.0f);
    //return float4(N * 0.5f + 0.5f, 1.0f);
    return float4(outColor, 1.0f);

}

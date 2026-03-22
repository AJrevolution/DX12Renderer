#include "Common.hlsli"

Texture2D g_GBuffer0 : register(t0, space0); // baseColor
Texture2D g_GBuffer1 : register(t1, space0); // normal
Texture2D g_GBuffer2 : register(t2, space0); // M/R/A
Texture2D g_Reserved : register(t3, space0);

SamplerState g_LinearClamp : register(s1);

cbuffer PerFrameConstants : register(b0)
{
    row_major float4x4 ViewProj;
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

float4 main(PSIn i) : SV_Target
{
    float3 base = g_GBuffer0.Sample(g_LinearClamp, i.uv).rgb;
    float3 N = normalize(g_GBuffer1.Sample(g_LinearClamp, i.uv).xyz);

    float3 L = SafeNormalize(-LightDir);
    float NdotL = saturate(dot(N, L));

    float3 diffuse = base * LightColor * NdotL;
    float3 ambient = base * 0.03f;

    float3 lit = diffuse + ambient;
    return float4(LinearToSRGB(lit), 1.0f);
}

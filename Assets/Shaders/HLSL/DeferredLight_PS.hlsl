#include "Common.hlsli"
#include "PBR.hlsli"

Texture2D g_GBuffer0 : register(t0, space0); // baseColor
Texture2D g_GBuffer1 : register(t1, space0); // normal
Texture2D g_GBuffer2 : register(t2, space0); // M/R/Ao
Texture2D g_Depth    : register(t3, space0); // depth SRV (R32_FLOAT)

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

    float3 ambient = base * (0.03f * ao);

    float3 lit = direct + ambient;
    return float4(LinearToSRGB(lit), 1.0f);
}

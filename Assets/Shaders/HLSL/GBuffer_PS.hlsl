Texture2D g_BaseColor : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MetalRough : register(t2, space1);

SamplerState g_LinearWrap : register(s0);

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
    float4 color : COLOR;
};

struct PSOut
{
    float4 rt0 : SV_Target0; // baseColor
    float4 rt1 : SV_Target1; // normal
    float4 rt2 : SV_Target2; // metal / rough / ao
};

float3 DecodeNormal(float3 n)
{
    return normalize(n * 2.0f - 1.0f);
}

PSOut main(PSIn i)
{
    PSOut o;

    float3 base = g_BaseColor.Sample(g_LinearWrap, i.uv).rgb;
    base *= BaseColorFactor.rgb;
    base *= i.color.rgb;

    float3 tangentNormal = DecodeNormal(g_NormalMap.Sample(g_LinearWrap, i.uv).xyz);

    float3 N = normalize(i.worldN);
    float3 T = normalize(i.worldT.xyz);
    float3 B = normalize(cross(N, T)) * i.worldT.w;
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(tangentNormal, TBN));

    float2 mr = g_MetalRough.Sample(g_LinearWrap, i.uv).gb;
    float roughness = saturate(mr.x * RoughnessFactor);
    float metallic = saturate(mr.y * MetallicFactor);

    o.rt0 = float4(base, 1.0f);
    o.rt1 = float4(worldNormal, 1.0f);
    o.rt2 = float4(metallic, roughness, 1.0f, 1.0f); // x=M, y=R, z=AO placeholder, w=1

    return o;
}

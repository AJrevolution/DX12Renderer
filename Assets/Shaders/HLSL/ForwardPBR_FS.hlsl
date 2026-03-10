#include "Common.hlsli"
#include "PBR.hlsli"

Texture2D g_BaseColor : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MetalRough : register(t2, space1); 

SamplerState g_LinearWrap : register(s0);

cbuffer PerFrameConstants : register(b0)
{
    float4x4 ViewProj;
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
    float4x4 World;
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
    float3 wpos : TEXCOORD0;
    float3 nrm : NORMAL; 
    float3 tan : TANGENT;
    float2 uv : TEXCOORD2;
    float4 col : COLOR;
};

float4 main(PSIn i) : SV_Target
{
    float3 base = g_BaseColor.Sample(g_LinearWrap, i.uv).rgb; // if SRV is _SRGB, this is already linear
    base *= BaseColorFactor.rgb;
    base *= i.col.rgb;

    float3 N = normalize(i.nrm);
    float3 T = normalize(i.tan);
    float3 B = normalize(cross(N, T)); // Gram-Schmidt or simple cross
    float3x3 TBN = float3x3(T, B, N);
    
    // Sample normal map and convert [0,1] range to [-1,1]
    float3 mapNormal = g_NormalMap.Sample(g_LinearWrap, i.uv).rgb * 2.0 - 1.0;
    // Rotate the map normal by our TBN matrix
    float3 worldNormal = normalize(mul(mapNormal, TBN));
    
    float3 V = SafeNormalize(CameraPos - i.wpos);
    float3 L = SafeNormalize(-LightDir); 
    
    PbrInputs p;
    p.N = worldNormal;
    p.V = V;
    p.L = L;
    p.albedo = base;
    p.metallic = saturate(MetallicFactor);
    p.roughness = saturate(RoughnessFactor);

    float3 lit = EvalDirectPBR(p, LightColor);

    // Temporary output transform since swapchain is UNORM (not SRGB)
    float3 outColor = LinearToSRGB(lit);
    return float4(outColor, 1.0f);

}

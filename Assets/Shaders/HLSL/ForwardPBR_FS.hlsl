#include "Common.hlsli"
#include "PBR.hlsli"

// Latlong IBL temp until cubemap/prefilter step.
// Scene table stays fixed in space0.

Texture2D g_BRDFLut : register(t0, space0);
Texture2D g_IBLDiffuse : register(t1, space0); 
Texture2D g_IBLSpecular : register(t2, space0); 
Texture2D g_ShadowMap : register(t3, space0); 

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
    row_major float4x4 InvViewProj;
    
    float3 CameraPos;
    float Time;
    
    uint FrameIndex;
    uint HasBRDFLut;    // Mapping to C++ hasBRDFLut
    uint HasIBL;        // Mapping to C++ hasIBL
    uint _pad0;

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

float2 DirToLatLongUV(float3 d)
{
    d = normalize(d);

    float u = atan2(d.z, d.x) / (2.0f * PI) + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / PI + 0.5f;

    return float2(u, 1.0f - v);
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

    float NdotV = saturate(dot(worldNormal, V));
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    
    float2 brdf;
    if (HasBRDFLut > 0)
    {
        brdf = g_BRDFLut.Sample(g_LinearClamp, float2(NdotV, roughness)).rg;
    }
    else
    {
        // Fallback: Use 1.0 for the scale and 0 for the bias. 
        // This keeps specular highlights visible but they won't be "physically correct."
        brdf = float2(1.0f, 0.0f);
    }
    
    // Temporary roughness approximation until proper prefilter+mips/cubemaps
    float3 diffuseEnv;
    float3 specularEnv;
    if (HasIBL > 0)
    {
        float2 diffuseUV = DirToLatLongUV(worldNormal);
        diffuseEnv = g_IBLDiffuse.Sample(g_LinearClamp, diffuseUV).rgb;

        float3 R = reflect(-V, worldNormal);
        float3 Rrough = normalize(lerp(R, worldNormal, roughness * roughness));
        float2 specUV0 = DirToLatLongUV(R);
        float2 specUV1 = DirToLatLongUV(Rrough);
        
        float3 specSharp = g_IBLSpecular.Sample(g_LinearClamp, specUV0).rgb;
        float3 specBlur = g_IBLSpecular.Sample(g_LinearClamp, specUV1).rgb;
        specularEnv = lerp(specSharp, specBlur, roughness);
    }
    else
    {
        // Fallback: A very dim gray for diffuse, and black for specular
        diffuseEnv = float3(0.03f, 0.03f, 0.03f);
        specularEnv = float3(0.0f, 0.0f, 0.0f);
    }
    float3 F = F_Schlick(NdotV, F0);
    float3 kd = (1.0f - F) * (1.0f - metallic);

    float3 iblDiffuse = kd * base * diffuseEnv;
    float3 iblSpec = specularEnv * (F0 * brdf.x + brdf.y);
    
    float3 lit = direct + iblDiffuse + iblSpec;

    // Temporary output transform since swapchain is UNORM (not SRGB)
    float3 outColor = LinearToSRGB(lit);

    return float4(outColor, 1.0f);

}

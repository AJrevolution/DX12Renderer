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
    row_major float4x4 LightViewProj;
    
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
    
    float2 ShadowInvSize;
    uint DebugView;
    uint RtSampleIndex;
    uint RtResetId;
    uint RtAccumulate;
    uint2 _padShadow;
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

float ComputeShadowFactor(float3 worldPos, float3 worldNormal, float3 lightDir)
{
    float4 shadowClip = mul(float4(worldPos, 1.0f), LightViewProj);

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUV = float2(
        shadowNdc.x * 0.5f + 0.5f,
        1.0f - (shadowNdc.y * 0.5f + 0.5f));

    float shadowDepth = shadowNdc.z;

    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f || shadowUV.y < 0.0f || shadowUV.y > 1.0f)
        return 1.0f;

    float bias = max(0.0005f, 0.002f * (1.0f - saturate(dot(worldNormal, -lightDir))));

    float visibility = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 uv = shadowUV + float2(x, y) * ShadowInvSize;
            float mapDepth = g_ShadowMap.Sample(g_PointClamp, uv).r;
            visibility += ((shadowDepth - bias) <= mapDepth) ? 1.0f : 0.0f;
        }
    }

    return visibility / 9.0f;
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
    
    //Shadow
    float shadowFactor = ComputeShadowFactor(i.worldPos, worldNormal, LightDir);
    direct *= shadowFactor;
    
    // Forward path is the baseline reference for deferred parity.
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

    if (DebugView == 1)
        return float4(worldNormal * 0.5f + 0.5f, 1.0f);

    if (DebugView == 2)
        return float4(roughness.xxx, 1.0f);

    if (DebugView == 3)
        return float4(metallic.xxx, 1.0f);

    if (DebugView == 4)
    {
        float depth01 = saturate(i.pos.z);
        return float4(depth01.xxx, 1.0f);
    }

    if (DebugView == 5)
        return float4(shadowFactor.xxx, 1.0f);

    return float4(outColor, 1.0f);

}

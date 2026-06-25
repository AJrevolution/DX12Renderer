#include "Common.hlsli"
#include "PBR.hlsli"
#include "MaterialSampling.hlsli"

// Latlong IBL temp until cubemap/prefilter step.
// Scene table stays fixed in space0.

Texture2D g_BRDFLut : register(t0, space0);
Texture2D g_IBLDiffuse : register(t1, space0); 
Texture2D g_IBLSpecular : register(t2, space0); 
Texture2D g_ShadowMap : register(t3, space0); 

Texture2D g_BaseColor : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MetalRough : register(t2, space1); 
Texture2D g_OcclusionMap : register(t3, space1);
Texture2D g_EmissiveMap : register(t4, space1);

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
    uint HasBRDFLut;
    uint HasIBL;
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
    uint RtEnableIndirect;
    float RtIndirectScale;
    
    uint PointLightCount;
    float3 PointLightPad;

    PointLightData PointLights[MAX_POINT_LIGHTS];
};

struct PSIn
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldN : TEXCOORD1;
    float4 worldT : TEXCOORD2;
    float2 uv0 : TEXCOORD3;
    float2 uv1 : TEXCOORD4;
    float4 color : COLOR;
};


float2 DirToLatLongUV(float3 d)
{
    d = normalize(d);

    float u = atan2(d.z, d.x) / (2.0f * PI) + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / PI + 0.5f;

    return float2(u, 1.0f - v);
}

float ComputeShadowFactor(float3 worldPos, float3 worldNormal, float3 lightDir)
{
    float4 shadowClip =
        mul(float4(worldPos, 1.0f), LightViewProj);

    if (abs(shadowClip.w) < 1e-6f)
        return 1.0f;

    float3 shadowNdc =
        shadowClip.xyz / shadowClip.w;

    float2 shadowUV = float2(
        shadowNdc.x * 0.5f + 0.5f,
        1.0f - (shadowNdc.y * 0.5f + 0.5f));

    float shadowDepth =
        shadowNdc.z;

    // Outside the light projection should be lit, not clamped to the edge
    // of the shadow map.
    if (shadowUV.x < 0.0f || shadowUV.x > 1.0f ||
        shadowUV.y < 0.0f || shadowUV.y > 1.0f ||
        shadowDepth < 0.0f || shadowDepth > 1.0f)
    {
        return 1.0f;
    }

    float ndotl =
        saturate(dot(worldNormal, -lightDir));

    float bias =
        max(0.0005f, 0.002f * (1.0f - ndotl));

    float visibility = 0.0f;
    float tapCount = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 uv =
                shadowUV + float2(x, y) * ShadowInvSize;

            // Important: PCF taps outside the map should also be treated as lit.
            // Otherwise g_PointClamp samples the shadow map edge and creates
            // rectangular/blocky border artifacts.
            if (uv.x < 0.0f || uv.x > 1.0f ||
                uv.y < 0.0f || uv.y > 1.0f)
            {
                visibility += 1.0f;
                tapCount += 1.0f;
                continue;
            }

            float mapDepth =
                g_ShadowMap.Sample(g_PointClamp, uv).r;

            visibility +=
                ((shadowDepth - bias) <= mapDepth) ? 1.0f : 0.0f;

            tapCount += 1.0f;
        }
    }

    return visibility / max(tapCount, 1.0f);
}

float4 main(PSIn i, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    // -------------------------------------------------------------------------
    // Base color + alpha mask
    // -------------------------------------------------------------------------

    float2 baseUv =
        SelectUv(BaseColorTexCoord, i.uv0, i.uv1);

    float4 baseSample =
        g_BaseColor.Sample(g_LinearWrap, baseUv);

    float alpha =
        baseSample.a * BaseColorFactor.a;

    ApplyAlphaMask(
        AlphaMode,
        alpha,
        EmissiveFactorAndAlphaCutoff.a);

    float3 base =
        baseSample.rgb *
        BaseColorFactor.rgb *
        i.color.rgb;

    // -------------------------------------------------------------------------
    // Double-sided geometry normal
    // -------------------------------------------------------------------------

    float faceSign =
        (DoubleSided != 0u && !isFrontFace) ? -1.0f : 1.0f;

    float3 N =
        normalize(i.worldN) * faceSign;

    float3 T =
        normalize(i.worldT.xyz);

    float3 B =
        normalize(cross(N, T)) * i.worldT.w;

    // -------------------------------------------------------------------------
    // Normal map
    // -------------------------------------------------------------------------

    float3 tangentNormal =
        float3(0.0f, 0.0f, 1.0f);

    if (HasNormalTexture != 0u)
    {
        float2 normalUv =
            SelectUv(NormalTexCoord, i.uv0, i.uv1);

        tangentNormal =
            DecodeNormalScaled(
                g_NormalMap.Sample(g_LinearWrap, normalUv).xyz,
                NormalScale);
    }

    float3x3 TBN =
        float3x3(T, B, N);

    float3 worldNormal =
        normalize(mul(tangentNormal, TBN));

    // -------------------------------------------------------------------------
    // Metallic / roughness
    // -------------------------------------------------------------------------

    float2 mrUv =
        SelectUv(MetalRoughTexCoord, i.uv0, i.uv1);

    float2 mr =
        g_MetalRough.Sample(g_LinearWrap, mrUv).gb;

    float roughness =
        saturate(mr.x * RoughnessFactor); // glTF roughness = G

    float metallic =
        saturate(mr.y * MetallicFactor); // glTF metallic = B

    // -------------------------------------------------------------------------
    // Occlusion
    // -------------------------------------------------------------------------

    float ao =
        1.0f;

    if (HasOcclusionTexture != 0u)
    {
        float2 aoUv =
            SelectUv(OcclusionTexCoord, i.uv0, i.uv1);

        float occlusionTexel =
            g_OcclusionMap.Sample(g_LinearWrap, aoUv).r;

        ao =
            ComputeMaterialOcclusion(
                occlusionTexel,
                OcclusionStrength);
    }

    // -------------------------------------------------------------------------
    // Emissive
    // -------------------------------------------------------------------------

    float3 emissiveTexel =
        1.0f.xxx;

    if (HasEmissiveTexture != 0u)
    {
        float2 emissiveUv =
            SelectUv(EmissiveTexCoord, i.uv0, i.uv1);

        emissiveTexel =
            g_EmissiveMap.Sample(g_LinearWrap, emissiveUv).rgb;
    }

    float3 emissive =
        ComputeEmissive(
            EmissiveFactorAndAlphaCutoff.rgb,
            HasEmissiveTexture,
            emissiveTexel);

    // -------------------------------------------------------------------------
    // Lighting inputs
    // -------------------------------------------------------------------------

    float3 V =
        SafeNormalize(CameraPos - i.worldPos);

    float3 L =
        SafeNormalize(-LightDir);

    PbrInputs p;
        p.N = worldNormal;
        p.V = V;
        p.L = L;
        p.albedo = base;
        p.metallic = metallic;
        p.roughness = roughness;
     
    // Directional sun.
    float3 direct = EvalDirectPBR(p, LightColor);

    // Directional shadow remains sun-only for now
    // Point-light shadows are intentionally not part of this phase.
    float shadowFactor = ComputeShadowFactor(i.worldPos, worldNormal, LightDir);
    direct *= shadowFactor;

    // Local point lights.
    uint pointLightCount = min(PointLightCount, (uint) MAX_POINT_LIGHTS);

    [loop]
    for (uint lightIndex = 0u; lightIndex < pointLightCount; ++lightIndex)
    {
        direct += EvalPointLightPBR(
            p,
            i.worldPos,
            PointLights[lightIndex]);
    }
    
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

    float3 iblDiffuse = kd * base * diffuseEnv * ao;
    float3 iblSpec = specularEnv * (F0 * brdf.x + brdf.y);
    
    float3 lit =
        direct +
        iblDiffuse +
        iblSpec +
        emissive;

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

#include "MaterialSampling.hlsli"

Texture2D g_BaseColor : register(t0, space1);
Texture2D g_NormalMap : register(t1, space1);
Texture2D g_MetalRough : register(t2, space1);
Texture2D g_OcclusionMap : register(t3, space1);
Texture2D g_EmissiveMap : register(t4, space1);

SamplerState g_LinearWrap : register(s0);


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

struct PSOut
{
    float4 rt0 : SV_Target0; // baseColor / alpha
    float4 rt1 : SV_Target1; // normal
    float4 rt2 : SV_Target2; // metal / rough / ao
    float4 rt3 : SV_Target3; // emissive / alpha
};

PSOut main(PSIn i, bool isFrontFace : SV_IsFrontFace)
{
    PSOut o;

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
        saturate(mr.x * RoughnessFactor);

    float metallic =
        saturate(mr.y * MetallicFactor);

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
    // GBuffer outputs
    // -------------------------------------------------------------------------

    o.rt0 = float4(base, alpha);
    o.rt1 = float4(worldNormal, 1.0f);
    o.rt2 = float4(metallic, roughness, ao, 1.0f);
    o.rt3 = float4(emissive, alpha);

    return o;
}

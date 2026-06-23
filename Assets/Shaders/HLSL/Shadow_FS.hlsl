#include "MaterialSampling.hlsli"

Texture2D g_BaseColor : register(t0, space1);
SamplerState g_LinearWrap : register(s0);

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

void main(PSIn i)
{
    float2 baseUv =
        SelectUv(BaseColorTexCoord, i.uv0, i.uv1);

    float alpha =
        g_BaseColor.Sample(g_LinearWrap, baseUv).a *
        BaseColorFactor.a;

    ApplyAlphaMask(
        AlphaMode,
        alpha,
        EmissiveFactorAndAlphaCutoff.a);
}
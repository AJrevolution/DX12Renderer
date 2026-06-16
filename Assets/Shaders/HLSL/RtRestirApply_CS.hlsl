Texture2D<float4> g_BaseDiffuse : register(t0);
Texture2D<float4> g_BaseSpec : register(t1);
Texture2D<float4> g_RestirDiffuse : register(t2);
Texture2D<float4> g_RestirSpec : register(t3);

RWTexture2D<float4> g_OutDiffuse : register(u0);
RWTexture2D<float4> g_OutSpec : register(u1);

cbuffer RtRestirApplyConstants : register(b0)
{
    float RtRestirApplyDiffuseScale;
    float RtRestirApplySpecularScale;
    uint RtRestirApplyMode;
    uint RtRestirApplyFlags;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;

    uint width;
    uint height;
    g_OutDiffuse.GetDimensions(width, height);

    if (p.x >= width || p.y >= height)
        return;

    float4 baseD = g_BaseDiffuse[p];
    float4 baseS = g_BaseSpec[p];

    float3 restirD = g_RestirDiffuse[p].rgb;
    float3 restirS = g_RestirSpec[p].rgb;

    if (RtRestirApplyMode == 0u)
    {
        g_OutDiffuse[p] = baseD;
        g_OutSpec[p] = baseS;
        return;
    }

    // Mode 1: validation-only additive post-denoise ReSTIR apply.
    // This is intentionally scaled because the resolved ReSTIR term is added
    // after the existing denoiser/combine path. A production integration should
    // apply before temporal/A-trous filtering and avoid this compensating scale.
    g_OutDiffuse[p] = float4(
    baseD.rgb + restirD * RtRestirApplyDiffuseScale,
    baseD.a);

    g_OutSpec[p] = float4(
    baseS.rgb + restirS * RtRestirApplySpecularScale,
    baseS.a);
}

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

bool Finite3(float3 value)
{
    return
        !any(isnan(value)) &&
        !any(isinf(value));
}

float3 SanitizeRadiance(float3 value)
{
    return Finite3(value)
        ? max(value, 0.0f.xxx)
        : 0.0f.xxx;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;

    uint width;
    uint height;
    g_OutDiffuse.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    const float4 baseDiffuse = g_BaseDiffuse[pixel];
    const float4 baseSpecular = g_BaseSpec[pixel];

    if (RtRestirApplyMode == 0u)
    {
        g_OutDiffuse[pixel] = baseDiffuse;
        g_OutSpec[pixel] = baseSpecular;
        return;
    }

    // Mode 1 is intentionally validation-only. Production integration belongs
    // before temporal/A-Trous filtering and replaces the legacy environment NEE
    // term rather than adding a post-denoise compensating signal.
    const float diffuseScale =
        max(0.0f, RtRestirApplyDiffuseScale);

    const float specularScale =
        max(0.0f, RtRestirApplySpecularScale);

    const float3 restirDiffuse =
        SanitizeRadiance(g_RestirDiffuse[pixel].rgb);

    const float3 restirSpecular =
        SanitizeRadiance(g_RestirSpec[pixel].rgb);

    g_OutDiffuse[pixel] = float4(
        SanitizeRadiance(baseDiffuse.rgb) + restirDiffuse * diffuseScale,
        baseDiffuse.a);

    g_OutSpec[pixel] = float4(
        SanitizeRadiance(baseSpecular.rgb) + restirSpecular * specularScale,
        baseSpecular.a);
}

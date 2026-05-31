#include "Common.hlsli"

Texture2D<float4> g_Diffuse : register(t0);
Texture2D<float4> g_Spec : register(t1);
Texture2D<float4> g_DiffuseAlbedo : register(t2);

RWTexture2D<float4> g_Output : register(u0);

cbuffer CombineConstants : register(b0)
{
    uint DiffuseIsDemodulated;
    uint3 _pad0;
};


[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 diff = g_Diffuse[pixel].rgb;
    float3 spec = g_Spec[pixel].rgb;
    
    if (DiffuseIsDemodulated != 0u)
    {
        float4 albedoSample = g_DiffuseAlbedo[pixel];

        if (albedoSample.a > 0.5f)
        {
            float3 albedo = saturate(albedoSample.rgb);
            diff *= albedo;
        }
    }

    float3 linearOut = diff + spec;
    g_Output[pixel] = float4(LinearToSRGB(linearOut), 1.0f);
}

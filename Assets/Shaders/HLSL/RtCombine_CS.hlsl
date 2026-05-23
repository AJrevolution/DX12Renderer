#include "Common.hlsli"

Texture2D<float4> g_Diffuse : register(t0);
Texture2D<float4> g_Spec : register(t1);

RWTexture2D<float4> g_Output : register(u0);

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

    g_Output[pixel] = float4(LinearToSRGB(diff + spec), 1.0f);
}

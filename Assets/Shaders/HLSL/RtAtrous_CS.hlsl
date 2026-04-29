#include "Common.hlsli"

Texture2D<float4> g_Signal : register(t0);
Texture2D<float4> g_Normal : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float2> g_Moments : register(t3);

RWTexture2D<float4> g_Output : register(u0);

SamplerState g_LinearClamp : register(s0);

cbuffer RtAtrousConstants : register(b0)
{
    float2 InvResolution;
    uint IterationIndex;
    uint StepWidth;

    float SigmaDepth;
    float SigmaNormal;
    float VarianceScale;
    uint UseMoments;

    uint FinalOutputSrgb;
    uint DebugView;
    uint2 _pad0;
};

float3 UnpackNormal(float4 packed)
{
    return SafeNormalize(packed.xyz * 2.0f - 1.0f);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    static const float kernel[3] = { 1.0f, 2.0f, 1.0f };

    float3 c0 = g_Signal[pixel].rgb;
    float4 n0Packed = g_Normal[pixel];
    float3 n0 = UnpackNormal(n0Packed);
    float rough0 = n0Packed.a;
    float z0 = g_Depth[pixel];
    
    float l0 = 0.0f;
    float sigmaL = 1.0f;

    if (UseMoments != 0)
    {
        float2 m0 = g_Moments[pixel];
        float variance = max(m0.y - m0.x * m0.x, 0.0f);
        sigmaL = max(1e-4f, VarianceScale * sqrt(variance));
        l0 = Luminance(c0);
    }
    
    if (DebugView == 28)
    {
        float protectionProxy = lerp(128.0f, 32.0f, rough0) / 128.0f;
        g_Output[pixel] = float4(protectionProxy.xxx, 1.0f);
        return;
    }
    float3 sum = 0.0f.xxx;
    float wsum = 0.0f;

    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            int2 offset = int2(x, y) * int(StepWidth);
            int2 p = int2(pixel) + offset;
            p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float3 c = g_Signal[uint2(p)].rgb;
            float4 nPacked = g_Normal[uint2(p)];
            float3 n = UnpackNormal(nPacked);
            float rough = nPacked.a;
            float z = g_Depth[uint2(p)];

            float ws = kernel[abs(x)] * kernel[abs(y)];

            //float nd = saturate(dot(n0, n));
            float roughMin = min(rough0, rough);
            float basePow = lerp(128.0f, 32.0f, roughMin);
            float normalScale = clamp(0.25f / max(1e-4f, SigmaNormal), 0.5f, 2.0f);
            float normalPow = basePow * normalScale;
            float wn = pow(saturate(dot(n0, n)), normalPow);

            float wz = exp(-abs(z0 - z) / max(1e-4f, SigmaDepth));

            float wl = 1.0f;
            if (UseMoments != 0)
            {
                float l = Luminance(c);
                float sigmaLUsed = max(1e-4f, sigmaL * lerp(0.5f, 1.0f, roughMin));
                wl = exp(-abs(l - l0) / sigmaLUsed);

            }

            float w = ws * wn * wz * wl;
            sum += c * w;
            wsum += w;
        }
    }

    float3 filtered = (wsum > 1e-6f) ? (sum / wsum) : c0;

    if (FinalOutputSrgb != 0)
        g_Output[pixel] = float4(LinearToSRGB(filtered), 1.0f);
    else
        g_Output[pixel] = float4(filtered, 1.0f);
}
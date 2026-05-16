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

    float LengthAttenuation;
    float LengthPower;
    uint FinalOutputSrgb;
    uint DebugView;
    
    float LengthSkipThreshold;
    uint EnableLengthSkip;
    uint2 _pad1;
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

    float4 centerSig = g_Signal[pixel];
    float3 c0 = centerSig.rgb;
    float len0 = centerSig.a;
    float lenN0 = saturate(len0 / 255.0f);

    float lenCurve = pow(lenN0, max(1e-4f, LengthPower));
    float wLenCenter = lerp(1.0f, LengthAttenuation, lenCurve);
    
    bool skipWide =
    (EnableLengthSkip != 0) &&
    (IterationIndex > 0) &&
    (lenN0 >= LengthSkipThreshold);
    
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
    
    if (DebugView == 43)
    {
        g_Output[pixel] = float4(wLenCenter.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 44)
    {
        g_Output[pixel] = float4(skipWide ? 1.0f.xxx : 0.0f.xxx, 1.0f);
        return;
    }

    if (skipWide)
    {
        if (FinalOutputSrgb != 0)
            g_Output[pixel] = float4(LinearToSRGB(c0), len0);
        else
            g_Output[pixel] = float4(c0, len0);

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

            float4 sig = g_Signal[uint2(p)];
            float3 c = sig.rgb;

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
            bool isCenterTap = (offset.x == 0 && offset.y == 0);

            float wLen = isCenterTap ? 1.0f : wLenCenter;
            
            float w = ws * wn * wz * wl * wLen;
            sum += c * w;
            wsum += w;
        }
    }

    float3 filtered = (wsum > 1e-6f) ? (sum / wsum) : c0;

    if (FinalOutputSrgb != 0)
        g_Output[pixel] = float4(LinearToSRGB(filtered), len0);
    else
        g_Output[pixel] = float4(filtered, len0);
}
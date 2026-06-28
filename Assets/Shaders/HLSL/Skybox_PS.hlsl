#include "Common.hlsli"

TextureCube<float4> g_DisplaySky : register(t0, space0);
SamplerState g_LinearClamp : register(s0);

cbuffer SkyConstants : register(b0)
{
    row_major float4x4 InvViewProj;

    float3 CameraPos;
    float DisplayIntensity;

    float RotationRadians;
    uint HasDisplaySky;
    uint _pad0;
    uint _pad1;

    float3 FallbackTopColor;
    float _pad2;

    float3 FallbackHorizonColor;
    float _pad3;

    float3 FallbackBottomColor;
    float _pad4;
};

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};


float3 ProceduralSky(float3 dir)
{
    float y =
        saturate(dir.y * 0.5f + 0.5f);

    float horizon =
        saturate(1.0f - abs(dir.y) * 3.0f);

    float3 base =
        lerp(FallbackBottomColor, FallbackTopColor, y);

    return lerp(
        base,
        FallbackHorizonColor,
        horizon * 0.65f);
}

float4 main(PSIn i) : SV_Target
{
    float2 ndc =
        float2(
            i.uv.x * 2.0f - 1.0f,
            (1.0f - i.uv.y) * 2.0f - 1.0f);

    float4 worldFar =
        mul(float4(ndc, 1.0f, 1.0f), InvViewProj);

    worldFar.xyz /=
        max(worldFar.w, 1.0e-6f);

    float3 dir =
        SafeNormalize(worldFar.xyz - CameraPos);

    dir =
        RotateY(dir, RotationRadians);

    float3 sky =
        HasDisplaySky != 0u
            ? g_DisplaySky.SampleLevel(g_LinearClamp, dir, 0.0f).rgb
            : ProceduralSky(dir);

    sky *=
        DisplayIntensity;

    return float4(LinearToSRGB(sky), 1.0f);
}
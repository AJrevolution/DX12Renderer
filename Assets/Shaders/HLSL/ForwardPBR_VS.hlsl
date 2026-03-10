#include "Common.hlsli"

cbuffer PerFrameConstants : register(b0)
{
    float4x4 ViewProj;
    float3 CameraPos;
    float Time;
    uint FrameIndex;
    float3 _pad0;

    float3 LightDir;
    float _pad1;
    float3 LightColor;
    float _pad2;
};

cbuffer PerDrawConstants : register(b1)
{
    float4x4 World;
    uint MaterialIndex;
    uint3 _padA;

    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float2 _padB;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL; 
    float3 tan : TANGENT; 
    float4 col : COLOR;
    float2 uv : TEXCOORD;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 wpos : TEXCOORD0;
    float3 nrm : NORMAL; 
    float3 tan : TANGENT;
    float2 uv : TEXCOORD2;
    float4 col : COLOR;
};

VSOut main(VSIn i)
{
    VSOut o;

    float4 wpos4 = mul(World, float4(i.pos, 1.0f));
    o.wpos = wpos4.xyz;

 
    // Transform N and T by World matrix (Direction only, so w=0)
    o.nrm = normalize(mul((float3x3) World, i.nrm)); // World space Normal
    o.tan = normalize(mul((float3x3) World, i.tan));

    o.pos = mul(ViewProj, wpos4);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

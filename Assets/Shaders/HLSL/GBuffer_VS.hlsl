#include "Common.hlsli"

cbuffer PerFrameConstants : register(b0)
{
    row_major float4x4 ViewProj;
    row_major float4x4 InvViewProj;
    row_major float4x4 LightViewProj;
    
    float3 CameraPos;
    float Time;
    uint FrameIndex;
    uint HasBRDFLut;
    uint HasIBL;
    uint _pad0;

    float3 LightDir;
    float pad1;
    float3 LightColor;
    float pad2;
    
    float2 ShadowInvSize;
    uint DebugView;
    uint _padShadow;
};

cbuffer PerDrawConstants : register(b1)
{
    row_major float4x4 World;
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
    float4 tan : TANGENT;
    float4 col : COLOR;
    float2 uv : TEXCOORD;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 worldN : TEXCOORD1;
    float4 worldT : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float4 color : COLOR;
};

VSOut main(VSIn i)
{
    VSOut o;

    float4 wpos = mul(float4(i.pos, 1.0f), World);
    o.worldPos = wpos.xyz;

    o.worldN = normalize(mul(float4(i.nrm, 0.0f), World).xyz);
    o.worldT.xyz = normalize(mul(float4(i.tan.xyz, 0.0f), World).xyz);
    o.worldT.w = i.tan.w;

    o.pos = mul(wpos, ViewProj);
    o.uv = i.uv;
    o.color = i.col;
    return o;
}

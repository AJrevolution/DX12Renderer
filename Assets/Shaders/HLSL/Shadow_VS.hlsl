#include "MaterialConstants.hlsli"

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
    float _pad1;
    float3 LightColor;
    float _pad2;
    
    float2 ShadowInvSize;
    uint DebugView;
    uint RtSampleIndex;
    uint RtResetId;
    uint RtAccumulate;
    uint RtEnableIndirect;
    float RtIndirectScale;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float4 tan : TANGENT;
    float4 col : COLOR;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

VSOut main(VSIn i)
{
    VSOut o;

    float4 worldPos = mul(float4(i.pos, 1.0f), World);
    o.pos = mul(worldPos, LightViewProj);
    o.uv0 = i.uv0;
    o.uv1 = i.uv1;
    
    return o;
}
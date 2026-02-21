// Register(b0) corresponds to Root Parameter 0.
// Note: Using Row-Major (DirectXMath) data from C++.
// Use mul(pos, ViewProj) for row-major math.
cbuffer PerFrameConstants : register(b0)
{
    float4x4 ViewProj;
    float3 CameraPos;
    float Time;
    uint FrameIndex;
};

struct VSIn 
{ 
    float3 pos : POSITION; 
    float4 col : COLOR; 
    float2 uv : TEXCOORD;
};
struct VSOut 
{ 
    float4 pos : SV_Position; 
    float4 col : COLOR;
    float2 uv : TEXCOORD;
};

VSOut main(VSIn i)
{
    VSOut o;
    // Use ViewProj from your cbuffer
    // Row-Major: float4 * matrix
    o.pos = mul(float4(i.pos, 1.0f), ViewProj);
    o.col = i.col;
    o.uv = i.uv; // Must pass UV through to the Pixel Shader
    return o;
}

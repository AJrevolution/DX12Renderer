// Register(b0) corresponds to Root Parameter 0.
// Note: Using Row-Major (DirectXMath) data from C++.
// Use mul(pos, ViewProj) for row-major math.
cbuffer PerFrameConstants : register(b0)
{
    float4x4 ViewProj; // 64 bytes
    float3 CameraPos; // 12 bytes
    float Time; // 4 bytes
    uint FrameIndex; // 4 bytes
};

struct VSIn { float3 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };

VSOut main(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0f), ViewProj);
    o.col = i.col;
    return o;
}

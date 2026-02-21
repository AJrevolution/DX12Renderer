Texture2D Tex0 : register(t0, space1); // Matches RootParam index 2
SamplerState Sampler : register(s0); // Matches Static Sampler

struct PSIn 
{
    float4 pos : SV_Position; 
    float4 col : COLOR; 
    float2 uv : TEXCOORD;
};

float4 main(PSIn i) : SV_Target
{
    float4 texColor = Tex0.Sample(Sampler, i.uv);
    return texColor * i.col;
}

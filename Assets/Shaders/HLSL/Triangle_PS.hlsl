struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };

float4 main(PSIn i) : SV_Target
{
    return i.col;
}

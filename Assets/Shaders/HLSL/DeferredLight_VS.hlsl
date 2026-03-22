struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID)
{
    VSOut o;

    float2 pos;
    if (vertexId == 0)
        pos = float2(-1.0, -1.0);
    else if (vertexId == 1)
        pos = float2(-1.0, 3.0);
    else
        pos = float2(3.0, -1.0);

    o.pos = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, 1.0 - (pos.y + 1.0) * 0.5);
    return o;
}

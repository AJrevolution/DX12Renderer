struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID)
{
    VSOut o;

    float2 pos =
        float2(
            (vertexId == 2) ? 3.0f : -1.0f,
            (vertexId == 1) ? 3.0f : -1.0f);

    o.pos = float4(pos, 1.0f, 1.0f);

    o.uv =
        float2(
            pos.x * 0.5f + 0.5f,
            1.0f - (pos.y * 0.5f + 0.5f));

    return o;
}
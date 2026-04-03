struct VertexRT
{
    float3 pos;
    float3 nrm;
    float4 tan;
    float4 color;
    float2 uv;
};

RaytracingAccelerationStructure g_Scene : register(t0);
RWTexture2D<float4> g_Output : register(u0);

StructuredBuffer<VertexRT> g_QuadVerts : register(t1);
ByteAddressBuffer g_QuadIndices : register(t2);
StructuredBuffer<VertexRT> g_FloorVerts : register(t3);
ByteAddressBuffer g_FloorIndices : register(t4);

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
    float2 _padShadow;
};

struct RayPayload
{
    float3 color;
    uint hit;
};

uint LoadIndex16(ByteAddressBuffer buf, uint idx)
{
    uint byteOffset = idx * 2;
    uint aligned = byteOffset & ~3;
    uint packed = buf.Load(aligned);
    return (byteOffset & 2) ? ((packed >> 16) & 0xFFFF) : (packed & 0xFFFF);
}

float3x4 GetObjectToWorld()
{
    return ObjectToWorld3x4();
}

float3 TransformNormal(float3 n)
{
    float3x3 m = (float3x3) GetObjectToWorld();
    return normalize(mul(n, m));
}

float3 BaryLerp(float3 a, float3 b, float3 c, float2 bary)
{
    float w = 1.0 - bary.x - bary.y;
    return a * w + b * bary.x + c * bary.y;
}

float3 FetchWorldNormal(uint instanceID, uint primitiveIndex, float2 bary)
{
    uint i0, i1, i2;
    float3 n0, n1, n2;

    if (instanceID == 0)
    {
        i0 = LoadIndex16(g_FloorIndices, primitiveIndex * 3 + 0);
        i1 = LoadIndex16(g_FloorIndices, primitiveIndex * 3 + 1);
        i2 = LoadIndex16(g_FloorIndices, primitiveIndex * 3 + 2);

        n0 = g_FloorVerts[i0].nrm;
        n1 = g_FloorVerts[i1].nrm;
        n2 = g_FloorVerts[i2].nrm;
    }
    else
    {
        i0 = LoadIndex16(g_QuadIndices, primitiveIndex * 3 + 0);
        i1 = LoadIndex16(g_QuadIndices, primitiveIndex * 3 + 1);
        i2 = LoadIndex16(g_QuadIndices, primitiveIndex * 3 + 2);

        n0 = g_QuadVerts[i0].nrm;
        n1 = g_QuadVerts[i1].nrm;
        n2 = g_QuadVerts[i2].nrm;
    }

    float3 n = normalize(BaryLerp(n0, n1, n2, bary));
    return TransformNormal(n);
}

float3 ShadeInstance(uint instanceID, float3 worldNormal)
{
    float3 base =
        (instanceID == 0) ? float3(0.8, 0.8, 0.8) :
        (instanceID == 1) ? float3(0.9, 0.9, 0.9) :
        (instanceID == 2) ? float3(0.85, 0.2, 0.2) :
                            float3(0.2, 0.3, 0.85);

    float3 L = normalize(-LightDir);
    float ndotl = saturate(dot(worldNormal, L));
    return base * (0.08 + ndotl);
}

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    float2 uv = (float2(pixel) + 0.5) / float2(dim);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float4 nearP = mul(float4(ndc, 0.0, 1.0), InvViewProj);
    float4 farP = mul(float4(ndc, 1.0, 1.0), InvViewProj);
    nearP.xyz /= nearP.w;
    farP.xyz /= farP.w;

    RayDesc ray;
    ray.Origin = nearP.xyz;
    ray.Direction = normalize(farP.xyz - nearP.xyz);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayPayload payload;
    payload.color = float3(0.02, 0.03, 0.05);
    payload.hit = 0;

    TraceRay(g_Scene, 0, 0xFF, 0, 1, 0, ray, payload);

    g_Output[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.color = float3(0.04, 0.06, 0.10);
    payload.hit = 0;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    const uint instanceID = InstanceID();
    const uint prim = PrimitiveIndex();

    float2 bary = float2(attr.barycentrics.x, attr.barycentrics.y);
    float3 worldNormal = FetchWorldNormal(instanceID, prim, bary);

    payload.color = ShadeInstance(instanceID, worldNormal);
    payload.hit = 1;
}
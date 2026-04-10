#include "Common.hlsli"
#include "PBR.hlsli"

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

struct RTInstanceData
{
    float4 baseColorFactor;
    float metallic;
    float roughness;
    float2 _pad0;
    uint meshType;
    uint materialId;
    uint2 _pad1;
};
StructuredBuffer<RTInstanceData> g_InstanceData : register(t5);
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

struct RayPayload
{
    float3 color;
    uint hit;
    uint isShadow;
    uint occluded;
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
    RTInstanceData inst = g_InstanceData[instanceID];

    uint i0, i1, i2;
    float3 n0, n1, n2;

    if (inst.meshType == 0)
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


float3 ShadeMaterial(
    float3 base,
    float metallic,
    float roughness,
    float3 worldPos,
    float3 worldNormal,
    float shadowVisibility)
{
    float3 L = normalize(-LightDir);
    float ndotl = saturate(dot(worldNormal, L));

    float3 ambient = base * 0.08f;
    float3 diffuseColor = base * (1.0f - metallic);

    float specPower = lerp(64.0f, 4.0f, roughness);
    float3 V = normalize(CameraPos - worldPos);
    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(worldNormal, H)), specPower);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    float3 specular = F0 * spec;

    float3 direct = (diffuseColor * ndotl + specular * ndotl) * LightColor * shadowVisibility;
    return ambient + direct;
}

float3 EvalSky(float3 dir)
{
    float t = saturate(dir.y * 0.5f + 0.5f);
    return lerp(float3(0.10f, 0.12f, 0.15f), float3(0.50f, 0.65f, 0.90f), t);
}

float3 HashColor(uint id)
{
    float3 p = frac(float3(0.1031, 0.11369, 0.13787) * float(id + 1));
    p += dot(p, p.yzx + 19.19);
    return frac((p.xxy + p.yzz) * p.zyx);
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
    payload.isShadow = 0;
    payload.occluded = 0;

    TraceRay(g_Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, payload);

    g_Output[pixel] = float4(LinearToSRGB(payload.color), 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    if (payload.isShadow == 1)
    {
        payload.occluded = 0;
        return;
    }
    
    payload.color = EvalSky(WorldRayDirection());
    payload.hit = 0;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (payload.isShadow == 1)
    {
        payload.occluded = 1;
        return;
    }
    

    const uint instanceID = InstanceID();
    const uint prim = PrimitiveIndex();

    float2 bary = float2(attr.barycentrics.x, attr.barycentrics.y);
    float3 worldNormal = FetchWorldNormal(instanceID, prim, bary);
    
    float3 worldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    if (dot(worldNormal, WorldRayDirection()) > 0.0f)
        worldNormal = -worldNormal;

    RTInstanceData data = g_InstanceData[instanceID];
    float3 base = data.baseColorFactor.rgb;
    float roughness = saturate(data.roughness);
    float metallic = saturate(data.metallic);
    
    roughness = max(0.045f, roughness);

    float3 V = SafeNormalize(CameraPos - worldPos);
    float3 L = SafeNormalize(-LightDir);
    
    RayPayload shadowPayload;
    shadowPayload.color = 0.0.xxx;
    shadowPayload.hit = 0;
    shadowPayload.isShadow = 1;
    shadowPayload.occluded = 0;

    float shadowBias = max(0.001f, 0.01f * (1.0f - saturate(dot(worldNormal, L))));

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + worldNormal * shadowBias;
    shadowRay.Direction = L;
    shadowRay.TMin = 0.0f;
    shadowRay.TMax = 1e38f;

    TraceRay(
        g_Scene,
         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF,
        0,
        1,
        0,
        shadowRay,
        shadowPayload);

    float shadowVisibility = (shadowPayload.occluded != 0) ? 0.0f : 1.0f;

    if (DebugView == 1)
    {
        payload.color = worldNormal * 0.5f + 0.5f;
        payload.hit = 1;
        return;
    }

    if (DebugView == 2)
    {
        payload.color = roughness.xxx;
        payload.hit = 1;
        return;
    }

    if (DebugView == 3)
    {
        payload.color = metallic.xxx;
        payload.hit = 1;
        return;
    }
    if (DebugView == 4)
    {
        float4 clip = mul(float4(worldPos, 1.0f), ViewProj);
        float depth01 = saturate((clip.z / clip.w) * 0.5f + 0.5f);
        payload.color = depth01.xxx;
        payload.hit = 1;
        return;
    }

    if (DebugView == 5)
    {
        payload.color = shadowVisibility.xxx;
        payload.hit = 1;
        return;
    }

    if (DebugView == 6)
    {
        payload.color = HashColor(instanceID);
        payload.hit = 1;
        return;
    }

    // use the same GGX direct-lighting helper as forward/deferred.
    PbrInputs p;
    p.N = worldNormal;
    p.V = V;
    p.L = L;
    p.albedo = base;
    p.metallic = metallic;
    p.roughness = roughness;

    float3 direct = EvalDirectPBR(p, LightColor) * shadowVisibility;
    float3 ambient = base * 0.03f;
    float3 lit = ambient + direct;

    payload.color = lit;
    payload.hit = 1;
}
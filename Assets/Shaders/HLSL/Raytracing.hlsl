#include "Common.hlsli"
#include "PBR.hlsli"

#define RT_MAX_MATERIALS 8
#define RT_TEXTURES_PER_MATERIAL 3
#define RT_TEXTURE_COUNT (RT_MAX_MATERIALS * RT_TEXTURES_PER_MATERIAL)

struct VertexRT
{
    float3 pos;
    float3 nrm;
    float4 tan;
    float4 color;
    float2 uv;
};

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
    uint RtSampleIndex;
    uint RtResetId;
    uint RtAccumulate;
    uint2 _padShadow;
};

struct RayPayload
{
    float3 color;
    uint hit;
    uint isShadow;
    uint occluded;
};

struct SurfaceBasisRT
{
    float2 uv;
    float3 worldGeomNormal;
    float3 worldTangent;
    float tangentSign;
};


RaytracingAccelerationStructure     g_Scene : register(t0);
RWTexture2D<float4>                 g_Output : register(u0);
RWTexture2D<float4>                 g_Accum : register(u1);
StructuredBuffer<VertexRT>          g_QuadVerts : register(t1);
ByteAddressBuffer                   g_QuadIndices : register(t2);
StructuredBuffer<VertexRT>          g_FloorVerts : register(t3);
ByteAddressBuffer                   g_FloorIndices : register(t4);
StructuredBuffer<RTInstanceData>    g_InstanceData : register(t5);

// t6.. = [base0, normal0, orm0, base1, normal1, orm1, ...]
Texture2D<float4>                   g_RtMaterialTextures[RT_TEXTURE_COUNT] : register(t6);

SamplerState                        g_LinearWrap : register(s0);
SamplerState                        g_LinearClamp : register(s1);

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

float3 TransformDirection(float3 v)
{
    float3x3 m = (float3x3) GetObjectToWorld();
    return SafeNormalize(mul(v, m));
}


float3 BaryLerp(float3 a, float3 b, float3 c, float2 bary)
{
    float w = 1.0 - bary.x - bary.y;
    return a * w + b * bary.x + c * bary.y;
}

float2 BaryLerp2(float2 a, float2 b, float2 c, float2 bary)
{
    float w = 1.0f - bary.x - bary.y;
    return a * w + b * bary.x + c * bary.y;
}

float3 BaryLerp3(float3 a, float3 b, float3 c, float2 bary)
{
    float w = 1.0f - bary.x - bary.y;
    return a * w + b * bary.x + c * bary.y;
}

float4 BaryLerp4(float4 a, float4 b, float4 c, float2 bary)
{
    float w = 1.0f - bary.x - bary.y;
    return a * w + b * bary.x + c * bary.y;
}

VertexRT LoadVertex(uint meshType, uint vertexIndex)
{
    if (meshType == 0)
        return g_FloorVerts[vertexIndex];
    else
        return g_QuadVerts[vertexIndex];
}

uint LoadMeshIndex(uint meshType, uint indexIndex)
{
    return (meshType == 0)
        ? LoadIndex16(g_FloorIndices, indexIndex)
        : LoadIndex16(g_QuadIndices, indexIndex);
}

void LoadTriangleVertices(uint meshType, uint primitiveIndex, out VertexRT v0, out VertexRT v1, out VertexRT v2)
{
    uint i0 = LoadMeshIndex(meshType, primitiveIndex * 3 + 0);
    uint i1 = LoadMeshIndex(meshType, primitiveIndex * 3 + 1);
    uint i2 = LoadMeshIndex(meshType, primitiveIndex * 3 + 2);

    v0 = LoadVertex(meshType, i0);
    v1 = LoadVertex(meshType, i1);
    v2 = LoadVertex(meshType, i2);
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

SurfaceBasisRT FetchSurfaceBasis(uint instanceID, uint primitiveIndex, float2 bary)
{
    RTInstanceData inst = g_InstanceData[instanceID];

    VertexRT v0, v1, v2;
    LoadTriangleVertices(inst.meshType, primitiveIndex, v0, v1, v2);

    float2 uv = BaryLerp2(v0.uv, v1.uv, v2.uv, bary);

    float3 nObj = SafeNormalize(BaryLerp3(v0.nrm, v1.nrm, v2.nrm, bary));
    float4 tObj4 = BaryLerp4(v0.tan, v1.tan, v2.tan, bary);
    float3 tObj = SafeNormalize(tObj4.xyz);

    SurfaceBasisRT s;
    s.uv = uv;
    s.worldGeomNormal = TransformDirection(nObj);
    s.worldTangent = TransformDirection(tObj);
    s.tangentSign = (tObj4.w >= 0.0f) ? 1.0f : -1.0f;
    return s;
}


uint GetRtTextureBaseIndex(uint materialId)
{
    uint clampedId = min(materialId, (uint) (RT_MAX_MATERIALS - 1));
    return clampedId * RT_TEXTURES_PER_MATERIAL;
}

float4 SampleRtTexture(uint materialId, uint slot, float2 uv)
{
    uint texIndex = GetRtTextureBaseIndex(materialId) + slot;
    return g_RtMaterialTextures[texIndex].SampleLevel(g_LinearWrap, uv, 0.0f);
}

float4 SampleBaseColorTex(uint materialId, float2 uv)
{
    return SampleRtTexture(materialId, 0, uv);
}

float3 SampleNormalTex(uint materialId, float2 uv)
{
    float3 s = SampleRtTexture(materialId, 1, uv).xyz;
    return SafeNormalize(s * 2.0f - 1.0f);
}

float3 SampleOrmTex(uint materialId, float2 uv)
{
    return SampleRtTexture(materialId, 2, uv).rgb;
}

uint HashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float NextRandom01(inout uint state)
{
    state = 1664525u * state + 1013904223u;
    return (state & 0x00FFFFFFu) / 16777216.0f;
}

float2 PixelJitter(uint2 pixel, uint sampleIndex, uint resetId)
{
    uint seed =
        pixel.x * 1973u ^
        pixel.y * 9277u ^
        sampleIndex * 26699u ^
        resetId * 31847u;

    seed = HashUint(seed);

    return float2(
        NextRandom01(seed),
        NextRandom01(seed)) - 0.5f;
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

    float2 jitter = 0.0f.xx;
    if (RtAccumulate != 0 && DebugView == 0)
    {
        jitter = PixelJitter(pixel, RtSampleIndex, RtResetId);
    }
    
    float2 uv = (float2(pixel) + 0.5 + jitter) / float2(dim);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float4 nearP = mul(float4(ndc, 0.0, 1.0), InvViewProj);
    float4 farP = mul(float4(ndc, 1.0, 1.0), InvViewProj);
    nearP.xyz /= nearP.w;
    farP.xyz /= farP.w;

    RayDesc ray;
    ray.Origin = nearP.xyz;
    ray.Direction = SafeNormalize(farP.xyz - nearP.xyz);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayPayload payload;
    payload.color = float3(0.02, 0.03, 0.05);
    payload.hit = 0;
    payload.isShadow = 0;
    payload.occluded = 0;

    TraceRay(g_Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, payload);
    
    float3 sampleColor = payload.color;

    // Debug views and non-accumulating mode bypass the running average.
    if (DebugView != 0 || RtAccumulate == 0)
    {
        g_Accum[pixel] = float4(sampleColor, 1.0f);
        g_Output[pixel] = float4(LinearToSRGB(sampleColor), 1.0f);
        return;
    }

    float sampleIndex = (float) RtSampleIndex;
    float3 oldAccum = (RtSampleIndex == 0) ? 0.0f.xxx : g_Accum[pixel].rgb;
    float3 avg = (oldAccum * sampleIndex + sampleColor) / (sampleIndex + 1.0f);

    g_Accum[pixel] = float4(avg, 1.0f);
    g_Output[pixel] = float4(LinearToSRGB(avg), 1.0f);
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
    
    float3 worldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
        
    SurfaceBasisRT surface = FetchSurfaceBasis(instanceID, prim, bary);
    
    float3 geomNormal = surface.worldGeomNormal;
    if (dot(geomNormal, WorldRayDirection()) > 0.0f)
        geomNormal = -geomNormal;

    RTInstanceData data = g_InstanceData[instanceID];

    float3 T = SafeNormalize(surface.worldTangent - geomNormal * dot(surface.worldTangent, geomNormal));
    float3 B = SafeNormalize(cross(geomNormal, T)) * surface.tangentSign;
    
    float3 tangentNormal = SampleNormalTex(data.materialId, surface.uv);
    float3 worldNormal = SafeNormalize(T * tangentNormal.x + B * tangentNormal.y + geomNormal * tangentNormal.z);
    
    if (dot(worldNormal, WorldRayDirection()) > 0.0f)
        worldNormal = -worldNormal;

    float4 baseTex = SampleBaseColorTex(data.materialId, surface.uv);
    float3 ormTex = SampleOrmTex(data.materialId, surface.uv);

    float3 base = data.baseColorFactor.rgb * baseTex.rgb;
    float roughness = saturate(data.roughness * ormTex.g);
    float metallic = saturate(data.metallic * ormTex.b);
    
    roughness = max(0.045f, roughness);
    
    float3 V = SafeNormalize(CameraPos - worldPos);
    float3 L = SafeNormalize(-LightDir);
    
    RayPayload shadowPayload;
    shadowPayload.color = 0.0.xxx;
    shadowPayload.hit = 0;
    shadowPayload.isShadow = 1;
    shadowPayload.occluded = 0;

    float shadowBias = max(0.001f, 0.01f * (1.0f - saturate(dot(geomNormal, L))));

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + geomNormal * shadowBias;
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
    
    if (DebugView == 7)
    {
        payload.color = base;
        payload.hit = 1;
        return;
    }

    if (DebugView == 8)
    {
        payload.color = float3(frac(surface.uv), 0.0f);
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
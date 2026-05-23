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
    uint RtEnableIndirect;
    float RtIndirectScale;
};

static const uint RT_RAY_PRIMARY_DIFFUSE = 0u;
static const uint RT_RAY_SHADOW = 1u;
static const uint RT_RAY_INDIRECT = 2u;
static const uint RT_RAY_PRIMARY_SPECULAR = 3u;

struct RayPayload
{
    float3 color; // returned radiance / signal
    uint rayType; // 0 = primary, 1 = shadow, 2 = indirect, 3 = primary specular
    uint occluded;
    uint rng;
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
RWTexture2D<float4>                 g_AccumDiff : register(u1);
RWTexture2D<float4>                 g_AccumSpec : register(u2);
RWTexture2D<float4>                 g_AovNormal : register(u3);
RWTexture2D<float>                  g_AovDepth : register(u4);
StructuredBuffer<VertexRT>          g_QuadVerts : register(t1);
ByteAddressBuffer                   g_QuadIndices : register(t2);
StructuredBuffer<VertexRT>          g_FloorVerts : register(t3);
ByteAddressBuffer                   g_FloorIndices : register(t4);
StructuredBuffer<RTInstanceData>    g_InstanceData : register(t5);

// t6.. = [base0, normal0, orm0, base1, normal1, orm1, ...]
Texture2D<float4>                   g_RtMaterialTextures[RT_TEXTURE_COUNT] : register(t6);
Texture2D<float4> g_BRDFLut : register(t30);
Texture2D<float4> g_IBLDiffuse : register(t31);
Texture2D<float4> g_IBLSpecular : register(t32);
//t1..t5 geometry + instance
//t6..t29 8 materials × 3 textures
//t30..t32 IBLresources
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

static const float kPi = 3.14159265f;

uint InitRng(uint2 pixel, uint sampleIndex, uint resetId)
{
    uint seed =
        pixel.x * 1973u ^
        pixel.y * 9277u ^
        sampleIndex * 26699u ^
        resetId * 31847u ^
        0x68bc21ebu;

    return HashUint(seed);
}

uint Lcg(inout uint s)
{
    s = 1664525u * s + 1013904223u;
    return s;
}

float Rand01(inout uint s)
{
    return (Lcg(s) & 0x00FFFFFFu) / 16777216.0f;
}

float3 SampleCosineHemisphere(float2 u)
{
    float r = sqrt(u.x);
    float phi = 2.0f * kPi * u.y;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0f, 1.0f - u.x));
    return float3(x, y, z);
}

void BuildOnb(float3 n, out float3 t, out float3 b)
{
    float3 up = (abs(n.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    t = SafeNormalize(cross(up, n));
    b = cross(n, t);
}

float Max3(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

float3 SampleGGXHalfVector(float2 u, float alpha)
{
    // Matches the same alpha convention used by EvalDirectPBR:
    // alpha = roughness^2, and D_GGX internally squares it again.
    float a2 = alpha * alpha;
    float phi = 2.0f * kPi * u.x;

    float cosTheta = sqrt(saturate((1.0f - u.y) / (1.0f + (a2 - 1.0f) * u.y)));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));

    return float3(
        sinTheta * cos(phi),
        sinTheta * sin(phi),
        cosTheta);
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

float3 EvalDirectAtSurface(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 L,
    float shadowVisibility)
{
    PbrInputs pbr;
    pbr.N = N;
    pbr.V = V;
    pbr.L = L;
    pbr.albedo = base;
    pbr.metallic = metallic;
    pbr.roughness = roughness;

    return EvalDirectPBR(pbr, LightColor) * shadowVisibility;
}

struct PbrSplit
{
    float3 diffuse;
    float3 spec;
};


PbrSplit EvalDirectPbrSplit(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 L)
{
    PbrSplit r;
    r.diffuse = 0.0f.xxx;
    r.spec = 0.0f.xxx;

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    if (NdotL <= 1e-4f || NdotV <= 1e-4f)
        return r;

    float3 H = SafeNormalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    float3 F = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotV, NdotL, roughness);

    float3 specBrdf = (D * G * F) / max(1e-4f, 4.0f * NdotV * NdotL);

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuseBrdf = kd * base / kPi;

    r.diffuse = diffuseBrdf * LightColor * NdotL;
    r.spec = specBrdf * LightColor * NdotL;
    return r;
}

float TraceShadowVisibility(float3 worldPos, float3 geomNormal, float3 L)
{
    RayPayload shadowPayload;
    shadowPayload.color = 0.0f.xxx;
    shadowPayload.rayType = RT_RAY_SHADOW;
    shadowPayload.occluded = 0;
    shadowPayload.rng = 0u;

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

    return (shadowPayload.occluded != 0) ? 0.0f : 1.0f;
}

float2 DirToLatLongUV(float3 d)
{
    d = SafeNormalize(d);
    float u = atan2(d.z, d.x) / (2.0f * kPi) + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / kPi + 0.5f;
    return float2(u, 1.0f - v);
}

float3 EvalIBLDiffuse(float3 N)
{
    float2 uv = DirToLatLongUV(N);
    return g_IBLDiffuse.SampleLevel(g_LinearClamp, uv, 0.0f).rgb;
}

float3 EvalIBLSpecular(float3 R, float3 Rrough, float roughness, float NdotV, float3 F0)
{
    float2 uvSharp = DirToLatLongUV(R);
    float2 uvBlur = DirToLatLongUV(Rrough);
    
    float3 specSharp = g_IBLSpecular.SampleLevel(g_LinearClamp, uvSharp, 0.0f).rgb;
    float3 specBlur = g_IBLSpecular.SampleLevel(g_LinearClamp, uvBlur, 0.0f).rgb;

    float3 prefiltered = lerp(specSharp, specBlur, roughness);

    float2 brdf = g_BRDFLut.SampleLevel(g_LinearClamp, float2(NdotV, roughness), 0.0f).rg;
    return prefiltered * (F0 * brdf.x + brdf.y);
}



// RT IBL parity target:
// - same latlong UV mapping convention as raster
// - same BRDF LUT usage
// - same roughness blur approximation (sharp R vs roughened R lerp)
// - same metallic/roughness interpretation from ORM

[shader("raygeneration")]
void RayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    
    bool isRtShadingDebug =
        (DebugView != 0 && DebugView <= 17) ||
        DebugView == 27;


    bool bypassAccum = (RtAccumulate == 0) || isRtShadingDebug;
    
    uint rng = InitRng(pixel, RtSampleIndex, RtResetId);

    // Keep camera jitter shared, but decorrelate the two primary signal traces.
    // Otherwise diffuse/spec indirect sampling can walk the same RNG sequence.
    uint diffRng = rng;
    uint specRng = HashUint(rng ^ 0x9E3779B9u ^ 0xD1B54A35u);
    
    float2 jitter = 0.0f.xx;
    if (!bypassAccum)
    {
        jitter = float2(Rand01(rng), Rand01(rng)) - 0.5f;
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

    float3 oldDiff =
        (!bypassAccum && RtSampleIndex != 0) ? g_AccumDiff[pixel].rgb : 0.0f.xxx;

    float3 oldSpec =
        (!bypassAccum && RtSampleIndex != 0) ? g_AccumSpec[pixel].rgb : 0.0f.xxx;
     

    RayPayload diffPayload;
    diffPayload.color = 0.0f.xxx;
    diffPayload.rayType = RT_RAY_PRIMARY_DIFFUSE;
    diffPayload.occluded = 0u;
    diffPayload.rng = diffRng;

    TraceRay(g_Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, diffPayload);

    // AOV debug views are produced by the diffuse primary trace.
    if (DebugView == 12)
    {
        float3 n = g_AovNormal[pixel].xyz;
        g_AccumDiff[pixel] = float4(n, 1.0f);
        g_AccumSpec[pixel] = float4(0.0f.xxx, 1.0f);
        g_Output[pixel] = float4(n, 1.0f);
        return;
    }
    
    if (DebugView == 13)
    {
        float d = g_AovDepth[pixel];
        g_AccumDiff[pixel] = float4(d.xxx, 1.0f);
        g_AccumSpec[pixel] = float4(0.0f.xxx, 1.0f);
        g_Output[pixel] = float4(d.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 27)
    {
        float r = g_AovNormal[pixel].w;
        g_AccumDiff[pixel] = float4(r.xxx, 1.0f);
        g_AccumSpec[pixel] = float4(0.0f.xxx, 1.0f);
        g_Output[pixel] = float4(r.xxx, 1.0f);
        return;
    }

    if (isRtShadingDebug)
    {
        float3 sampleColor = diffPayload.color;
        g_AccumDiff[pixel] = float4(sampleColor, 1.0f);
        g_AccumSpec[pixel] = float4(0.0f.xxx, 1.0f);
        g_Output[pixel] = float4(LinearToSRGB(sampleColor), 1.0f);
        return;
    }
    
    RayPayload specPayload;
    specPayload.color = 0.0f.xxx;
    specPayload.rayType = RT_RAY_PRIMARY_SPECULAR;
    specPayload.occluded = 0u;
    specPayload.rng = specRng;

    TraceRay(
    g_Scene,
    RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
    0xFF,
    0,
    1,
    0,
    ray,
    specPayload);
    
    float3 sampleDiff = diffPayload.color;
    float3 sampleSpec = specPayload.color;
    
    if (bypassAccum)
    {
        g_AccumDiff[pixel] = float4(sampleDiff, 1.0f);
        g_AccumSpec[pixel] = float4(sampleSpec, 1.0f);
        
        if (DebugView == 48)
        {
            g_Output[pixel] = float4(LinearToSRGB(sampleDiff), 1.0f);
            return;
        }

        if (DebugView == 49)
        {
            g_Output[pixel] = float4(LinearToSRGB(sampleSpec), 1.0f);
            return;
        }

        if (DebugView == 50)
        {
            g_Output[pixel] = float4(LinearToSRGB(sampleDiff + sampleSpec), 1.0f);
            return;
        }
        
        g_Output[pixel] = float4(LinearToSRGB(sampleDiff + sampleSpec), 1.0f);
        return;
    }
    float sampleIndex = (float) RtSampleIndex;
    
    float3 avgDiff = (oldDiff * sampleIndex + sampleDiff) / (sampleIndex + 1.0f);
    float3 avgSpec = (oldSpec * sampleIndex + sampleSpec) / (sampleIndex + 1.0f);
    
    g_AccumDiff[pixel] = float4(avgDiff, 1.0f);
    g_AccumSpec[pixel] = float4(avgSpec, 1.0f);
    

    if (DebugView == 48)
    {
        g_Output[pixel] = float4(LinearToSRGB(avgDiff), 1.0f);
        return;
    }
    else if (DebugView == 49)
    {
        g_Output[pixel] = float4(LinearToSRGB(avgSpec), 1.0f);
        return;
    }
    else if (DebugView == 50)
    {
        g_Output[pixel] = float4(LinearToSRGB(avgDiff + avgSpec), 1.0f);
        return;
    }

    g_Output[pixel] = float4(LinearToSRGB(avgDiff + avgSpec), 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    if (payload.rayType == RT_RAY_SHADOW)
    {
        payload.occluded = 0u;
        return;
    }
    
    float3 sky = EvalSky(WorldRayDirection());
    
    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE)
    {
        uint2 pixel = DispatchRaysIndex().xy;
       
        g_AovNormal[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_AovDepth[pixel] = 1.0f;
        
        payload.color = sky;
        return;
    }
    
    if (payload.rayType == RT_RAY_PRIMARY_SPECULAR)
    {
        payload.color = 0.0f.xxx;
        return;
    }
    payload.color = sky;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (payload.rayType == RT_RAY_SHADOW)
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
    
    // Use outgoing direction for both primary and indirect hits.
    float3 V = SafeNormalize(-WorldRayDirection());
    float3 L = SafeNormalize(-LightDir);
    
    float4 clip = mul(float4(worldPos, 1.0f), ViewProj);
    float depth01 = saturate(clip.z / clip.w);
    
    float3 diffuseAlbedo = base * (1.0f - metallic);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    float NdotV = saturate(dot(worldNormal, V));
    float3 R = reflect(-V, worldNormal);
    float3 Rrough = SafeNormalize(lerp(R, worldNormal, roughness * roughness));
    
    float3 iblDiffuse = 0.0f.xxx;
    float3 iblSpecular = 0.0f.xxx;

    if (HasIBL != 0 && HasBRDFLut != 0)
    {
        iblDiffuse = EvalIBLDiffuse(worldNormal) * diffuseAlbedo;
        iblSpecular = EvalIBLSpecular(R, Rrough, roughness, NdotV, F0);
    }

    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE)
    {
        uint2 pixel = DispatchRaysIndex().xy;
        g_AovNormal[pixel] = float4(geomNormal * 0.5f + 0.5f, roughness);
        g_AovDepth[pixel] = depth01;
    }
    
    if (payload.rayType == RT_RAY_INDIRECT)
    {
        payload.color = EvalDirectAtSurface(
            base,
            metallic,
            roughness,
            worldNormal,
            V,
            L,
            1.0f);

        return;
    }
    
    float shadowVisibility = TraceShadowVisibility(worldPos, geomNormal, L);

    if (DebugView == 1)
    {
        payload.color = worldNormal * 0.5f + 0.5f;
        return;
    }

    if (DebugView == 2)
    {
        payload.color = roughness.xxx;
        return;
    }

    if (DebugView == 3)
    {
        payload.color = metallic.xxx;
        return;
    }
    if (DebugView == 4)
    {
        payload.color = depth01.xxx;
        return;
    }

    if (DebugView == 5)
    {
        payload.color = shadowVisibility.xxx;
        return;
    }

    if (DebugView == 6)
    {
        payload.color = HashColor(instanceID);
        return;
    }
    
    if (DebugView == 7)
    {
        payload.color = base;
        return;
    }

    if (DebugView == 8)
    {
        payload.color = float3(frac(surface.uv), 0.0f);
        return;
    }

    float3 indirectDiffuse = 0.0f.xxx;
    float3 indirectSpec = 0.0f.xxx;
    float3 lobeVis = 0.0f.xxx;

    bool allowIndirect =
        (RtEnableIndirect != 0) &&
        (
            (RtAccumulate != 0 && DebugView == 0) ||
            DebugView == 9 ||
            DebugView == 10 ||
            DebugView == 11 ||
            DebugView == 48 ||
            DebugView == 49 ||
            DebugView == 50
        );

    if (allowIndirect)
    {
        //float3 diffuseAlbedo = base * (1.0f - metallic);
        //float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
        float pSpec = clamp(Max3(F0), 0.05f, 0.95f);

        float xi = Rand01(payload.rng);
        bool chooseSpec = (xi < pSpec);

        // DebugView 11: red = diffuse lobe, blue = specular lobe.
        lobeVis = chooseSpec ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);

        float3 bounceT, bounceB;
        BuildOnb(worldNormal, bounceT, bounceB);

        RayPayload bounce;
        bounce.color = 0.0f.xxx;
        bounce.rayType = RT_RAY_INDIRECT;
        bounce.occluded = 0;
        bounce.rng = payload.rng;

        RayDesc r;
        r.Origin = worldPos + geomNormal * 0.001f;
        r.TMin = 0.0f;
        r.TMax = 1e38f;

        if (chooseSpec)
        {
            float alpha = max(roughness * roughness, 0.002f);

            float2 u = float2(Rand01(payload.rng), Rand01(payload.rng));
            float3 localH = SampleGGXHalfVector(u, alpha);
            float3 H = SafeNormalize(
                bounceT * localH.x +
                bounceB * localH.y +
                worldNormal * localH.z);

            float3 wi = SafeNormalize(reflect(-V, H));

            float NdotV = saturate(dot(worldNormal, V));
            float NdotL = saturate(dot(worldNormal, wi));
            float NdotH = saturate(dot(worldNormal, H));
            float VdotH = saturate(dot(V, H));

            if (NdotV > 1e-4f && NdotL > 1e-4f && NdotH > 1e-4f && VdotH > 1e-4f)
            {
                r.Direction = wi;

                TraceRay(
                    g_Scene,
                    RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                    0xFF,
                    0,
                    1,
                    0,
                    r,
                    bounce);

                float G = G_Smith(NdotV, NdotL, roughness);
                float3 F = F_Schlick(VdotH, F0);

                // Simplified GGX importance-sampling estimator.
                float3 specWeight =
                    (G * F * VdotH) / max(1e-4f, NdotV * NdotH);

                // Firefly control.
                specWeight = min(specWeight, float3(10.0f, 10.0f, 10.0f));
                float3 bounceRadiance = min(bounce.color, float3(20.0f, 20.0f, 20.0f));

                indirectSpec = bounceRadiance * specWeight / pSpec;
                payload.rng = bounce.rng;
            }
        }
        else
        {
            float2 u = float2(Rand01(payload.rng), Rand01(payload.rng));
            float3 localDir = SampleCosineHemisphere(u);
            float3 wi = SafeNormalize(
                bounceT * localDir.x +
                bounceB * localDir.y +
                worldNormal * localDir.z);

            float NdotL = saturate(dot(worldNormal, wi));
            if (NdotL > 1e-4f)
            {
                r.Direction = wi;

                TraceRay(
                    g_Scene,
                    RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
                    0xFF,
                    0,
                    1,
                    0,
                    r,
                    bounce);

                indirectDiffuse =
                    bounce.color * diffuseAlbedo / max(1e-4f, 1.0f - pSpec);

                payload.rng = bounce.rng;
            }
        }
    }

    float3 indirect = indirectDiffuse + indirectSpec;
    
    if (DebugView == 9)
    {
        payload.color = RtIndirectScale * indirect;
        return;
    }
    
    if (DebugView == 10)
    {
        // Specular indirect only.
        payload.color = RtIndirectScale * indirectSpec;
        return;
    }

    if (DebugView == 11)
    {
        // Red = diffuse branch, Blue = specular branch.
        payload.color = lobeVis;
        return;
    }
    
    if (DebugView == 14)
    {
        payload.color = iblDiffuse;
        return;
    }

    if (DebugView == 15)
    {
        payload.color = iblSpecular;
        return;
    }
    
    if (DebugView == 16)
    {
        float2 uvDbg = DirToLatLongUV(worldNormal);
        payload.color = float3(frac(uvDbg), 0.0f);
        return;
    }

    if (DebugView == 17)
    {
        payload.color = Rrough * 0.5f + 0.5f;
        return;
    }
    
    PbrSplit directSplit = EvalDirectPbrSplit(
        base,
        metallic,
        roughness,
        worldNormal,
        V,
        L);


    float3 ambient = base * 0.03f;
    
    float3 sampleDiffuse =
        ambient +
        directSplit.diffuse * shadowVisibility +
        iblDiffuse +
        RtIndirectScale * indirectDiffuse;

    float3 sampleSpec =
        directSplit.spec * shadowVisibility +
        iblSpecular +
        RtIndirectScale * indirectSpec;

    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE)
    {
        payload.color = sampleDiffuse;
        return;
    }

    if (payload.rayType == RT_RAY_PRIMARY_SPECULAR)
    {
        payload.color = sampleSpec;
        return;
    }

    payload.color = sampleDiffuse + sampleSpec;
}

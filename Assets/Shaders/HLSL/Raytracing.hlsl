#include "Common.hlsli"
#include "PBR.hlsli"
#include "RtSampling.hlsli"
#include "RtReservoir.hlsli"

#define RT_MAX_MATERIALS 64
#define RT_TEXTURES_PER_MATERIAL 5
#define RT_TEXTURE_COUNT (RT_MAX_MATERIALS * RT_TEXTURES_PER_MATERIAL)

#define RT_TEXTURE_SLOT_BASE_COLOR 0
#define RT_TEXTURE_SLOT_NORMAL 1
#define RT_TEXTURE_SLOT_METAL_ROUGH 2
#define RT_TEXTURE_SLOT_OCCLUSION 3
#define RT_TEXTURE_SLOT_EMISSIVE 4

#define ALPHA_MODE_OPAQUE 0u
#define ALPHA_MODE_MASK   1u
#define ALPHA_MODE_BLEND  2u

#define RT_MESH_FLOOR 0
#define RT_MESH_QUAD 1
#define RT_MESH_IMPORTED 2

#define RT_MAX_IMPORTED_MODEL_BUFFERS 16

struct VertexRT
{
    float3 pos;
    float3 nrm;
    float4 tan;
    float4 color;
    float2 uv0;
    float2 uv1;
};

struct RTInstanceData
{
    float4 baseColorFactor;
    float4 emissiveFactorAndAlphaCutoff;

    float metallic;
    float roughness;
    float occlusionStrength;
    float normalScale;

    uint alphaMode;
    uint doubleSided;
    uint hasOcclusionTexture;
    uint hasEmissiveTexture;

    uint baseColorTexCoord;
    uint normalTexCoord;
    uint metalRoughTexCoord;
    uint occlusionTexCoord;

    uint emissiveTexCoord;
    uint hasNormalTexture;

    uint meshType;

    uint materialId;
    
    uint meshBufferId;

    uint objectId;

    uint indexStart;

    uint _pad0;

    row_major float4x4 prevObjectToWorld;
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
    
    uint PointLightCount;
    float3 PointLightPad;

    PointLightData PointLights[MAX_POINT_LIGHTS];
    
    float IblIntensity;
    float IblRotationRadians;
    uint HasLightingEnvironment;
    uint _padIbl;
};

struct RtSkyConstants
{
    uint enabled;
    uint hasDisplaySky;
    uint visibleInDxr;
    uint specularMissUsesDisplaySky;

    float displayIntensity;
    float rotationRadians;
    uint2 pad0;

    float4 fallbackTopColor;
    float4 fallbackHorizonColor;
    float4 fallbackBottomColor;
};

struct RtEnvironmentLightingConstants
{
    uint hasRadianceTexture;
    uint hasLightingEnvironment;
    uint2 pad0;

    float lightingIntensity;
    float lightingRotationRadians;
    uint2 pad1;
};

cbuffer RtRayGenConstants : register(b1)
{
    row_major float4x4 RtPrevViewProj;
    row_major float4x4 RtCurrViewProj;
    float4 RtPrevCameraPos;
    float4 RtCurrCameraPos;
    
    uint RtHasPrevMotion;
    uint3 _RtRayGenPad0;
    
    // RT environment sampling contract:
    //
    // This is a DXR integrator feature only.
    // It must not modify the denoiser stack, temporal histories, raster IBL,
    // BRDF model, or any post-filter shaping.
    //
    // The alias table stores final solid-angle PDFs per cubemap texel.
    // SampleEnvironment() and PdfEnvironment() must use matching cubemap
    // face/UV mapping and the same solid-angle convention.
    uint RtEnvSamplingMode;
    uint RtUseEnvImportanceSampling;
    uint RtUseEnvMIS;
    uint RtEnvAliasCount;

    uint RtEnvFaceSize;
    uint RtEnvAliasFallback;
    uint RtSamplingDebugView;
    uint RtUseEnvNeeForFinal;

    float RtEnvIntensity;
    float RtEnvPdfEpsilon;
    float RtEnvDeltaRoughnessCutoff;
    float RtEnvMISPower;
    
    uint RtEnvNeeFireflyGuard;
    uint RtEnvAliasVersion;
    float RtEnvNeeMaxRadiance;
    float _padRtSampling1;
    
    uint RtEnableRestirEnvDi;
    uint RtRestirInitialCandidateCount;
    uint RtRestirDebugView;
    uint RtRestirDispatchMode;

    float RtRestirMaxM;
    float RtRestirMaxAge;
    float RtRestirMinTarget;
    float RtRestirMaxWeight;
    
    RtSkyConstants RtSky;
    RtEnvironmentLightingConstants RtEnvironment;
    
};

static const uint RT_RAY_PRIMARY_DIFFUSE = 0u;
static const uint RT_RAY_SHADOW = 1u;
static const uint RT_RAY_INDIRECT = 2u;
static const uint RT_RAY_PRIMARY_SPECULAR = 3u;
static const float RT_VIEWZ_INVALID = DISTANCE_INVALID;
static const float RT_VIEWZ_VIS_MAX = 25.0f;
static const uint RT_SURFACE_ID_INVALID = 0xFFFFFFFFu;
static const float RT_ALBEDO_STABLE_MIN = 0.03f;
static const float RT_ALBEDO_STABLE_MAX = 0.97f;
static const uint RT_ENV_SAMPLING_BRDF_ONLY = 0u;
static const uint RT_ENV_SAMPLING_ENV_ONLY = 1u;
static const uint RT_ENV_SAMPLING_MIS_ONE_SAMPLE = 2u;
static const uint RT_ENV_SAMPLING_MIS_TWO_SAMPLE = 3u;

struct RayPayload
{
    float3 color; // returned radiance / signal
    uint rayType; // 0 = primary, 1 = shadow, 2 = indirect, 3 = primary specular

    // Shadow rays:
    //   1 = occluded, 0 = visible.
    //
    // Radiance rays:
    //   1 = hit geometry, 0 = missed into environment.
    uint occluded;

    uint rng;
};

struct SurfaceBasisRT
{
    float2 uv0;
    float2 uv1;
    float3 objPos;
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
RWTexture2D<float2>                 g_AovMotion : register(u5);         // prevUV, (-1,-1) invalid
RWTexture2D<float>                  g_AovViewZRaw : register(u6);       // primary ray distance / ViewZ-compatible guide, -1 invalid
RWTexture2D<uint>                   g_AovSurfaceId : register(u7);      // object/material id, 0xFFFFFFFF invalid
RWTexture2D<float4>                 g_AovDiffuseAlbedo : register(u8);  // rgb=diffuse albedo, a=stable demod flag
RWStructuredBuffer<RtRestirReservoir> g_RestirInitialReservoir : register(u9);
RWTexture2D<float4>                 g_RestirResolvedDiffuse : register(u10);
RWTexture2D<float4>                 g_RestirResolvedSpec : register(u11);
StructuredBuffer<VertexRT>          g_QuadVerts : register(t1);
ByteAddressBuffer                   g_QuadIndices : register(t2);
StructuredBuffer<VertexRT>          g_FloorVerts : register(t3);
ByteAddressBuffer                   g_FloorIndices : register(t4);
StructuredBuffer<RTInstanceData>    g_InstanceData : register(t5);

// t6..t325 = material texture table:
// 64 materials × 5 textures:
// [baseColor, normal, metallicRoughness, occlusion, emissive]
Texture2D<float4>                   g_RtMaterialTextures[RT_TEXTURE_COUNT] : register(t6);
StructuredBuffer<VertexRT>          g_ImportedVerts[RT_MAX_IMPORTED_MODEL_BUFFERS] : register(t330);
ByteAddressBuffer                   g_ImportedIndices[RT_MAX_IMPORTED_MODEL_BUFFERS] : register(t346);

Texture2D<float4>                   g_BRDFLut : register(t380);
Texture2D<float4>                   g_IBLDiffuse : register(t381);
Texture2D<float4>                   g_IBLSpecular : register(t382); 
StructuredBuffer<RtEnvAliasEntry>   g_EnvAlias : register(t383);
StructuredBuffer<RtRestirReservoir> g_RestirResolveReservoir : register(t384);
TextureCube<float4>                 g_RtDisplaySky : register(t385);
Texture2D<float4>                   g_RtEnvironmentRadiance : register(t386);
SamplerState                        g_LinearWrap : register(s0);
SamplerState                        g_LinearClamp : register(s1);

uint LoadIndex16(ByteAddressBuffer buf, uint idx)
{
    uint byteOffset = idx * 2;
    uint aligned = byteOffset & ~3;
    uint packed = buf.Load(aligned);
    return (byteOffset & 2) ? ((packed >> 16) & 0xFFFF) : (packed & 0xFFFF);
}

uint LoadIndex32(ByteAddressBuffer buffer, uint indexIndex)
{
    return buffer.Load(indexIndex * 4u);
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

float2 SelectRtUv(uint texCoord, float2 uv0, float2 uv1)
{
    return texCoord == 1u ? uv1 : uv0;
}

uint LoadImportedIndex32(
    RTInstanceData instanceData,
    uint localIndex)
{
    const uint bufferId =
        min(instanceData.meshBufferId, RT_MAX_IMPORTED_MODEL_BUFFERS - 1u);

    return g_ImportedIndices[NonUniformResourceIndex(bufferId)].Load(
        (instanceData.indexStart + localIndex) * 4u);
}

VertexRT LoadVertex(
    RTInstanceData instanceData,
    uint vertexIndex)
{
    if (instanceData.meshType == RT_MESH_FLOOR)
    {
        return g_FloorVerts[vertexIndex];
    }

    if (instanceData.meshType == RT_MESH_QUAD)
    {
        return g_QuadVerts[vertexIndex];
    }

    const uint bufferId =
        min(instanceData.meshBufferId, RT_MAX_IMPORTED_MODEL_BUFFERS - 1u);

    return g_ImportedVerts[NonUniformResourceIndex(bufferId)][vertexIndex];
}

void LoadTriangleVertices(
    RTInstanceData instanceData,
    uint primitiveIndex,
    out VertexRT v0,
    out VertexRT v1,
    out VertexRT v2)
{
    const uint i0Local = primitiveIndex * 3u + 0u;
    const uint i1Local = primitiveIndex * 3u + 1u;
    const uint i2Local = primitiveIndex * 3u + 2u;

    uint i0 = 0u;
    uint i1 = 0u;
    uint i2 = 0u;

    if (instanceData.meshType == RT_MESH_FLOOR)
    {
        i0 = LoadIndex16(g_FloorIndices, i0Local);
        i1 = LoadIndex16(g_FloorIndices, i1Local);
        i2 = LoadIndex16(g_FloorIndices, i2Local);
    }
    else if (instanceData.meshType == RT_MESH_QUAD)
    {
        i0 = LoadIndex16(g_QuadIndices, i0Local);
        i1 = LoadIndex16(g_QuadIndices, i1Local);
        i2 = LoadIndex16(g_QuadIndices, i2Local);
    }
    else
    {
        i0 = LoadImportedIndex32(instanceData, i0Local);
        i1 = LoadImportedIndex32(instanceData, i1Local);
        i2 = LoadImportedIndex32(instanceData, i2Local);
    }

    v0 = LoadVertex(instanceData, i0);
    v1 = LoadVertex(instanceData, i1);
    v2 = LoadVertex(instanceData, i2);
}

struct SurfaceUvsRT
{
    float2 uv0;
    float2 uv1;
};

SurfaceUvsRT FetchSurfaceUvs(
    RTInstanceData instanceData,
    uint primitiveIndex,
    float2 bary)
{
    VertexRT v0, v1, v2;
    LoadTriangleVertices(instanceData, primitiveIndex, v0, v1, v2);

    SurfaceUvsRT uvs;
    uvs.uv0 = BaryLerp2(v0.uv0, v1.uv0, v2.uv0, bary);
    uvs.uv1 = BaryLerp2(v0.uv1, v1.uv1, v2.uv1, bary);
    return uvs;
}

SurfaceBasisRT FetchSurfaceBasis(uint instanceID, uint primitiveIndex, float2 bary)
{
    RTInstanceData inst = g_InstanceData[instanceID];

    VertexRT v0, v1, v2;
    LoadTriangleVertices(inst, primitiveIndex, v0, v1, v2);

    float2 uv0 = BaryLerp2(v0.uv0, v1.uv0, v2.uv0, bary);
    float2 uv1 = BaryLerp2(v0.uv1, v1.uv1, v2.uv1, bary);
    float3 objPos = BaryLerp3(v0.pos, v1.pos, v2.pos, bary);
    
    float3 nObj = SafeNormalize(BaryLerp3(v0.nrm, v1.nrm, v2.nrm, bary));
    float4 tObj4 = BaryLerp4(v0.tan, v1.tan, v2.tan, bary);
    float3 tObj = SafeNormalize(tObj4.xyz);

    SurfaceBasisRT s;
    s.uv0 = uv0;
    s.uv1 = uv1;
    s.objPos = objPos;
    s.worldGeomNormal = TransformDirection(nObj);
    s.worldTangent = TransformDirection(tObj);
    s.tangentSign = (tObj4.w >= 0.0f) ? 1.0f : -1.0f;
    return s;
}

uint GetRtTextureBaseIndex(uint materialId)
{
    const uint clampedId =
        min(materialId, (uint) (RT_MAX_MATERIALS - 1));

    return clampedId * RT_TEXTURES_PER_MATERIAL;
}

float4 SampleRtTexture(uint materialId, uint slot, float2 uv)
{
    const uint texIndex = GetRtTextureBaseIndex(materialId) + slot;
    return g_RtMaterialTextures[NonUniformResourceIndex(texIndex)].SampleLevel(g_LinearWrap, uv, 0.0f);
}

float4 SampleBaseColorTex(uint materialId, float2 uv)
{
    return SampleRtTexture(
        materialId,
        RT_TEXTURE_SLOT_BASE_COLOR,
        uv);
}

float3 SampleNormalTex(uint materialId, float2 uv)
{
    const float3 texel =
        SampleRtTexture(
            materialId,
            RT_TEXTURE_SLOT_NORMAL,
            uv).xyz;

    return SafeNormalize(texel * 2.0f - 1.0f);
}

float3 SampleMetalRoughTex(uint materialId, float2 uv)
{
    return SampleRtTexture(
        materialId,
        RT_TEXTURE_SLOT_METAL_ROUGH,
        uv).rgb;
}

float ComputeRtMaterialOcclusion(
    RTInstanceData instanceData,
    float2 uv)
{
    if (instanceData.hasOcclusionTexture == 0u)
        return 1.0f;

    const float occlusionTexel =
        SampleRtTexture(
            instanceData.materialId,
            RT_TEXTURE_SLOT_OCCLUSION,
            uv).r;

    return saturate(
        1.0f +
        saturate(instanceData.occlusionStrength) *
        (occlusionTexel - 1.0f));
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

struct RtEnvRandoms
{
    float2 uEnvSelect;
    float2 uEnvTexel;
    float uTechnique;
};

uint InitRtEnvRng(uint2 pixel, uint lobeId, uint bounceId)
{
    uint seed = InitRng(pixel, RtSampleIndex, RtResetId);

    seed ^= FrameIndex * 0xA511E9B3u;
    seed ^= lobeId * 0x9E3779B9u;
    seed ^= bounceId * 0x63D83595u;
    seed ^= 0xD1B54A35u;

    return HashUint(seed);
}

RtEnvRandoms MakeRtEnvRandoms(uint lobeId, uint bounceId)
{
    uint2 pixel = DispatchRaysIndex().xy;

    uint s = InitRtEnvRng(pixel, lobeId, bounceId);

    RtEnvRandoms r;
    r.uEnvSelect = float2(Rand01(s), Rand01(s));
    r.uEnvTexel = float2(Rand01(s), Rand01(s));
    r.uTechnique = Rand01(s);

    return r;
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

float3 EvalPointLightsAtSurface(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 worldPos)
{
    PbrInputs pbr;
    pbr.N = N;
    pbr.V = V;
    pbr.L = 0.0f.xxx;
    pbr.albedo = base;
    pbr.metallic = metallic;
    pbr.roughness = roughness;

    float3 result = 0.0f.xxx;

    uint pointLightCount = min(PointLightCount, (uint) MAX_POINT_LIGHTS);

    [loop]
    for (uint lightIndex = 0u; lightIndex < pointLightCount; ++lightIndex)
    {
        result += EvalPointLightPBR(
            pbr,
            worldPos,
            PointLights[lightIndex]);
    }

    return result;
}

float3 ComputeRtEmissive(RTInstanceData data, float2 uv)
{
    float3 e = data.emissiveFactorAndAlphaCutoff.rgb;

    if (data.hasEmissiveTexture != 0u)
    {
        const uint textureBase = GetRtTextureBaseIndex(data.materialId);

        const uint emissiveTexIndex = textureBase + RT_TEXTURE_SLOT_EMISSIVE;
        
        e *= g_RtMaterialTextures[NonUniformResourceIndex(emissiveTexIndex)].SampleLevel(g_LinearWrap, uv, 0.0f).rgb;
    }

    return e;
}

struct PbrSplit
{
    float3 diffuse;
    float3 spec;
};


PbrSplit EvalDirectPbrSplitWithRadiance(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 L,
    float3 lightRadiance)
{
    PbrSplit r;
    r.diffuse = 0.0f.xxx;
    r.spec = 0.0f.xxx;

    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));

    if (NdotL <= 1e-4f || NdotV <= 1e-4f)
    {
        return r;
    }

    float3 H = SafeNormalize(V + L);

    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float rough = saturate(roughness);
    float metal = saturate(metallic);
    float alpha = max(rough * rough, 0.002f);

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metal);
    float3 F = F_Schlick(VdotH, F0);

    float D = D_GGX(NdotH, alpha);
    float G = G_Smith(NdotV, NdotL, rough);

    float3 specBrdf =
    (D * G * F) /
    max(1e-4f, 4.0f * NdotV * NdotL);

    float3 kd = (1.0f - F) * (1.0f - metal);
    float3 diffuseBrdf = kd * base / kPi;

    r.diffuse = diffuseBrdf * lightRadiance * NdotL;
    r.spec = specBrdf * lightRadiance * NdotL;

    return r;
}

PbrSplit EvalDirectPbrSplit(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 L)
{
    return EvalDirectPbrSplitWithRadiance(
        base,
        metallic,
        roughness,
        N,
        V,
        L,
        LightColor);
}

PbrSplit EvalPointLightsPbrSplitAtSurface(
    float3 base,
    float metallic,
    float roughness,
    float3 N,
    float3 V,
    float3 worldPos)
{
    PbrSplit r;
    r.diffuse = 0.0f.xxx;
    r.spec = 0.0f.xxx;

    uint pointLightCount = min(PointLightCount, (uint) MAX_POINT_LIGHTS);

    [loop]
    for (uint lightIndex = 0u; lightIndex < pointLightCount; ++lightIndex)
    {
        PointLightData light = PointLights[lightIndex];

        float3 toLight = light.position - worldPos;
        float distanceSq = dot(toLight, toLight);

        if (distanceSq <= 1e-6f)
        {
            continue;
        }

        float rangeSq = light.range * light.range;

        if (distanceSq >= rangeSq)
        {
            continue;
        }

        float3 L = toLight * rsqrt(distanceSq);

        float attenuation =
            PointLightAttenuation(distanceSq, light.range);

        float3 radiance =
            max(light.color, 0.0f.xxx) *
            max(light.intensity, 0.0f) *
            attenuation;

        PbrSplit s =
            EvalDirectPbrSplitWithRadiance(
                base,
                metallic,
                roughness,
                N,
                V,
                L,
                radiance);

        r.diffuse += s.diffuse;
        r.spec += s.spec;
    }

    return r;
}

PbrSplit EvalEnvBrdfSplit(
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

    if (NdotH <= 1e-4f || VdotH <= 1e-4f)
        return r;

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    float3 F = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, roughness);
    float G = G_Smith(NdotV, NdotL, roughness);

    // BRDF only. Caller multiplies by Li and NdotL.
    r.spec = (D * G * F) / max(1e-4f, 4.0f * NdotV * NdotL);

    float3 kd = (1.0f - F) * (1.0f - metallic);
    r.diffuse = kd * base / kPi;

    return r;
}

float PdfDiffuseBrdf(float3 N, float3 L)
{
    float NdotL = saturate(dot(N, L));
    return NdotL / kPi;
}

float PdfSpecularGGXBrdf(
    float roughness,
    float3 N,
    float3 V,
    float3 L)
{
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));

    if (NdotL <= 1e-4f || NdotV <= 1e-4f)
        return 0.0f;

    float3 H = SafeNormalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    if (NdotH <= 1e-4f || VdotH <= 1e-4f)
        return 0.0f;

    float D = D_GGX(NdotH, roughness);

    // Half-vector GGX PDF converted to reflected-direction PDF.
    return (D * NdotH) / max(1e-4f, 4.0f * VdotH);
}

bool EnvSamplingReady()
{
    return
        RtUseEnvImportanceSampling != 0u &&
        RtEnvAliasCount != 0u &&
        RtEnvFaceSize != 0u;
}

uint EffectiveEnvSamplingMode()
{
    // Keep one-sample MIS mapped to the validated reference estimator for now.
    // A true one-sample estimator should be added only after the two-sample
    // reference path matches energy.
    if (RtEnvSamplingMode == RT_ENV_SAMPLING_MIS_ONE_SAMPLE)
        return RT_ENV_SAMPLING_MIS_TWO_SAMPLE;

    return RtEnvSamplingMode;
}

bool EnvNeeEnabled()
{
    if (!EnvSamplingReady())
        return false;

    // Do not replace smooth raster-style IBL with a noisy uniform fallback sample.
    // Keep fallback only for table/debug validation.
    if (RtEnvAliasFallback != 0u)
        return false;

    uint mode = EffectiveEnvSamplingMode();

    return
        mode == RT_ENV_SAMPLING_ENV_ONLY ||
        mode == RT_ENV_SAMPLING_MIS_TWO_SAMPLE;
}

bool IsRtEnvEstimatorDebugView(uint dv)
{
    return
        dv == 98u ||
        dv == 99u ||
        dv == 100u ||
        dv == 101u ||
        dv == 102u;
}

bool EnvBrdfEnvironmentContributionEnabled(bool isSpecular, float roughness)
{
    if (!EnvSamplingReady())
        return true;

    if (RtUseEnvNeeForFinal == 0u)
        return true;

    uint mode = EffectiveEnvSamplingMode();

    if (mode == RT_ENV_SAMPLING_ENV_ONLY)
        return false;

    return true;
}

bool EnvNeeDebugReady()
{
    return
        EnvSamplingReady() &&
        RtEnvAliasFallback == 0u;
}

bool EnvBrdfEnvironmentMisEnabled(
    bool isSpecular,
    float roughness,
    uint samplingDebugView)
{
    if (!EnvSamplingReady())
        return false;

    if (RtEnvAliasFallback != 0u)
        return false;

    if (RtUseEnvMIS == 0u && samplingDebugView != 101u)
        return false;

    const bool useEnvHitMisForFinal =
        RtUseEnvNeeForFinal != 0u;

    const bool useEnvHitMisForDebug =
        samplingDebugView == 101u;

    if (!useEnvHitMisForFinal && !useEnvHitMisForDebug)
        return false;

    uint mode = EffectiveEnvSamplingMode();

    if (mode != RT_ENV_SAMPLING_MIS_TWO_SAMPLE &&
        !useEnvHitMisForDebug)
    {
        return false;
    }

    if (isSpecular && roughness < RtEnvDeltaRoughnessCutoff)
        return false;

    return true;
}

float TraceShadowVisibility(float3 worldPos, float3 geomNormal, float3 L)
{
    RayPayload shadowPayload;
    shadowPayload.color = 0.0f.xxx;
    shadowPayload.rayType = RT_RAY_SHADOW;
    shadowPayload.occluded = 0;
    shadowPayload.rng = 0u;

    float NoL = dot(geomNormal, L);

    float3 biasNormal =
    (NoL >= 0.0f) ? geomNormal : -geomNormal;

    float shadowBias =
    max(0.001f, 0.01f * (1.0f - saturate(abs(NoL))));

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + biasNormal * shadowBias;
    shadowRay.Direction = L;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = 1e38f;

    TraceRay(
        g_Scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        0,
        1,
        0,
        shadowRay,
        shadowPayload);

    return (shadowPayload.occluded != 0) ? 0.0f : 1.0f;
}

bool TraceEnvironmentVisibility(float3 worldPos, float3 geomNormal, float3 wi)
{
    RayPayload shadowPayload;
    shadowPayload.color = 0.0f.xxx;
    shadowPayload.rayType = RT_RAY_SHADOW;
    shadowPayload.occluded = 0u;
    shadowPayload.rng = 0u;

    float NoW = dot(geomNormal, wi);

    float3 biasNormal =
        (NoW >= 0.0f) ? geomNormal : -geomNormal;

    float visibilityBias =
        max(0.001f, 0.01f * (1.0f - saturate(abs(NoW))));

    RayDesc shadowRay;
    shadowRay.Origin = worldPos + biasNormal * visibilityBias;
    shadowRay.Direction = wi;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = 1.0e30f;

    TraceRay(
        g_Scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        0,
        1,
        0,
        shadowRay,
        shadowPayload);

    return shadowPayload.occluded == 0u;
}

float3 WorldToLightingEnvDir(float3 worldDir)
{
    return RotateY(
        SafeNormalize(worldDir),
        RtEnvironment.lightingRotationRadians);
}

float3 LightingEnvToWorldDir(float3 envDir)
{
    return RotateY(
        SafeNormalize(envDir),
        -RtEnvironment.lightingRotationRadians);
}

float2 DirToLatLongUV(float3 d)
{
    d = SafeNormalize(d);
    float u = atan2(d.z, d.x) / (2.0f * kPi) + 0.5f;
    float v = asin(clamp(d.y, -1.0f, 1.0f)) / kPi + 0.5f;
    return float2(u, 1.0f - v);
}

float2 LightingEnvUV(float3 worldDir)
{
    return DirToLatLongUV(
        WorldToLightingEnvDir(worldDir));
}

float3 LookupEnvironmentRadiance(float3 worldDir)
{
    float2 uv =
        LightingEnvUV(worldDir);

    float3 radiance =
        RtEnvironment.hasRadianceTexture != 0u
            ? g_RtEnvironmentRadiance.SampleLevel(g_LinearWrap, uv, 0.0f).rgb
            : g_IBLSpecular.SampleLevel(g_LinearWrap, uv, 0.0f).rgb;

    return radiance * RtEnvironment.lightingIntensity;
}

bool IsFiniteScalarRt(float v)
{
    return !isnan(v) && !isinf(v);
}

bool IsFinite3Rt(float3 v)
{
    return
        IsFiniteScalarRt(v.x) &&
        IsFiniteScalarRt(v.y) &&
        IsFiniteScalarRt(v.z);
}

EnvSample SampleEnvironment(float2 uSelect, float2 uTexel)
{
    EnvSample s;
    s.wi = 0.0f.xxx;
    s.Li = 0.0f.xxx;
    s.pdf = 0.0f;
    s.index = 0u;
    s.valid = false;

    if (RtUseEnvImportanceSampling == 0u ||
        RtEnvAliasCount == 0u ||
        RtEnvFaceSize == 0u)
    {
        return s;
    }

    float aliasCountF = float(RtEnvAliasCount);

    // Avoid u == 1 mapping outside the table.
    float ux = min(saturate(uSelect.x), 0.99999994f);
    float uf = ux * aliasCountF;

    uint idx = min(uint(uf), RtEnvAliasCount - 1u);
    float aliasXi = min(saturate(uSelect.y), 0.99999994f);

    RtEnvAliasEntry e = g_EnvAlias[idx];

    uint chosen = (aliasXi < e.q) ? idx : e.alias;
    chosen = min(chosen, RtEnvAliasCount - 1u);

    RtEnvAliasEntry c = g_EnvAlias[chosen];

    uint faceSize2 = RtEnvFaceSize * RtEnvFaceSize;
    if (faceSize2 == 0u)
        return s;

    uint face = chosen / faceSize2;
    uint rem = chosen - face * faceSize2;

    uint y = rem / RtEnvFaceSize;
    uint x = rem - y * RtEnvFaceSize;

    float2 texelXi = saturate(uTexel);

    float2 cubeUv =
        (float2(float(x), float(y)) + texelXi) /
        float(RtEnvFaceSize);

    float3 envDir =
        CubeFaceUVToDirection(face, cubeUv);

    float3 wi =
        LightingEnvToWorldDir(envDir);

    float3 Li =
        LookupEnvironmentRadiance(wi);

    s.wi = wi;
    s.Li = Li;
    s.pdf = c.pdf;
    s.index = chosen;
    s.valid =
        IsFinite3Rt(Li) &&
        all(Li >= 0.0f.xxx) &&
        c.pdf > RtEnvPdfEpsilon;

    return s;
}

float PdfEnvironment(float3 wi)
{
    if (RtUseEnvImportanceSampling == 0u ||
        RtEnvAliasCount == 0u ||
        RtEnvFaceSize == 0u)
    {
        return 0.0f;
    }

    float3 envDir =
        WorldToLightingEnvDir(wi);

    float2 cubeUv;
    uint face =
        DirectionToCubeFaceUV(
            SafeNormalize(envDir),
            cubeUv);

    uint x = min(
        uint(cubeUv.x * float(RtEnvFaceSize)),
        RtEnvFaceSize - 1u);

    uint y = min(
        uint(cubeUv.y * float(RtEnvFaceSize)),
        RtEnvFaceSize - 1u);

    uint idx =
        face * RtEnvFaceSize * RtEnvFaceSize +
        y * RtEnvFaceSize +
        x;

    if (idx >= RtEnvAliasCount)
        return 0.0f;

    return max(0.0f, g_EnvAlias[idx].pdf);
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

float3 TransformPrevObjectToWorld(RTInstanceData data, float3 objPos)
{
    return mul(float4(objPos, 1.0f), data.prevObjectToWorld).xyz;
}

float2 ProjectPrevUVFromWorld(float3 prevWorldPos)
{
    float4 prevClip = mul(float4(prevWorldPos, 1.0f), RtPrevViewProj);

    if (prevClip.w <= 1e-6f)
        return float2(-1.0f, -1.0f);

    float2 prevNdc = prevClip.xy / prevClip.w;
    return float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
}

bool PrevUVValid(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

bool ViewZValid(float z)
{
    return DistanceValid(z);
}

bool SurfaceIdValid(uint id)
{
    return id != RT_SURFACE_ID_INVALID;
}

uint MakeSurfaceId(uint objectId, uint materialId)
{
    // SurfaceId is used for same-frame guide validation and cross-frame temporal
    // validation. objectId must be stable for the logical draw/object, not just
    // the current TLAS instance ordering.
    uint obj = objectId & 0xFFFFu;
    uint mat = materialId & 0xFFFFu;
    return obj | (mat << 16);
}

uint HashSurfaceId(uint id)
{
    uint h = id;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

float SurfaceIdToGray(uint id)
{
    if (!SurfaceIdValid(id))
        return 0.0f;

    uint h = HashSurfaceId(id);
    float u = float(h & 0x00FFFFFFu) / 16777215.0f;

    // Keep invalid as black, but make every valid surface visible.
    return lerp(0.12f, 1.0f, u);
}

bool DiffuseAlbedoStable(float3 albedo)
{
    float minA = min(albedo.r, min(albedo.g, albedo.b));
    float maxA = max(albedo.r, max(albedo.g, albedo.b));

    return minA > RT_ALBEDO_STABLE_MIN &&
           maxA < RT_ALBEDO_STABLE_MAX;
}

float3 SurfaceIdToColor(uint id)
{
    if (!SurfaceIdValid(id))
        return 0.0f.xxx;

    uint h = HashSurfaceId(id);

    float r = float((h >> 0) & 0xFFu) / 255.0f;
    float g = float((h >> 8) & 0xFFu) / 255.0f;
    float b = float((h >> 16) & 0xFFu) / 255.0f;

    // Keep all valid IDs visible, including ID/hash values near zero.
    return lerp(0.12f.xxx, 1.0f.xxx, float3(r, g, b));
}

bool IsRtSamplingDebugView(uint dv)
{
    return
        dv == 96u ||
        dv == 97u ||
        dv == 98u ||
        dv == 99u ||
        dv == 100u ||
        dv == 101u ||
        dv == 102u ||
        dv == 103u;
}

float RtDebugLuminance(float3 c)
{
    return dot(max(c, 0.0f.xxx), float3(0.2126f, 0.7152f, 0.0722f));
}

float3 RtHeat(float v)
{
    v = saturate(v);

    // Simple blue/cyan/yellow/red-style ramp without relying on external helpers.
    return saturate(float3(
        smoothstep(0.35f, 1.00f, v),
        smoothstep(0.10f, 0.80f, v) * (1.0f - smoothstep(0.85f, 1.00f, v)),
        1.0f - smoothstep(0.00f, 0.65f, v)));
}

float3 RtSamplingFallbackMask()
{
    bool invalid =
        RtUseEnvImportanceSampling == 0u ||
        RtEnvAliasCount == 0u ||
        RtEnvFaceSize == 0u ||
        RtEnvAliasFallback != 0u;

    return invalid ? float3(1.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
}

bool IsRtRestirInitialDebugView(uint dv)
{
    return
        dv == 104u ||
        dv == 105u;
}

bool IsRtRestirResolveDebugView(uint dv)
{
    return
        dv == 110u ||
        dv == 111u ||
        dv == 112u ||
        dv == 113u ||
        dv == 114u;
}

bool IsRtRestirTemporalDebugView(uint dv)
{
    return dv == 106u || dv == 107u;
}

bool IsRtRestirSpatialDebugView(uint dv)
{
    return dv == 108u || dv == 109u;
}

bool IsRtRestirDebugView(uint dv)
{
    return
        IsRtRestirInitialDebugView(dv) ||
        IsRtRestirTemporalDebugView(dv) ||
        IsRtRestirSpatialDebugView(dv) ||
        IsRtRestirResolveDebugView(dv);
}

bool RestirInitialEnabled()
{
    return
        RtRestirDispatchMode == 0u &&
        (
            RtEnableRestirEnvDi != 0u ||
            IsRtRestirInitialDebugView(RtRestirDebugView)
        );
}

float3 ProceduralRtSky(float3 dir)
{
    float y =
        saturate(dir.y * 0.5f + 0.5f);

    float horizon =
        saturate(1.0f - abs(dir.y) * 3.0f);

    float3 base =
        lerp(
            RtSky.fallbackBottomColor.rgb,
            RtSky.fallbackTopColor.rgb,
            y);

    return lerp(
        base,
        RtSky.fallbackHorizonColor.rgb,
        horizon * 0.65f);
}

float3 SampleRtDisplaySky(float3 dir)
{
    dir =
        RotateY(
            SafeNormalize(dir),
            RtSky.rotationRadians);

    float3 sky =
        RtSky.hasDisplaySky != 0u
            ? g_RtDisplaySky.SampleLevel(g_LinearClamp, dir, 0.0f).rgb
            : ProceduralRtSky(dir);

    return sky * RtSky.displayIntensity;
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
    
    uint pixelIndex = pixel.y * dim.x + pixel.x;

    bool restirInitialEnabled = RestirInitialEnabled();
    bool isRtRestirInitialDebug = IsRtRestirInitialDebugView(RtRestirDebugView);

    if (restirInitialEnabled)
    {
        RtRestirReservoir empty;
        ReservoirClear(empty);
        g_RestirInitialReservoir[pixelIndex] = empty;
    }
    
    uint samplingDebugView = RtSamplingDebugView;
    bool isRtSamplingDebug = IsRtSamplingDebugView(samplingDebugView);

    bool isRtShadingDebug =
        (DebugView != 0 && DebugView <= 17) ||
        DebugView == 27 ||
        isRtSamplingDebug;

    bool isMotionDebug =
        DebugView == 51 ||
        DebugView == 52 ||
        DebugView == 53;
    
    bool isViewZDebug =
        DebugView == 61 ||
        DebugView == 62;
    
    bool isSurfaceIdDebug =
        DebugView == 65 ||
        DebugView == 66 ||
        DebugView == 74 ||
        DebugView == 75;
    
    bool isDiffuseAlbedoDebug =
        DebugView == 67 ||
        DebugView == 68;
    
    bool isRtRestirResolveDispatch =
        RtRestirDispatchMode == 1u;

    bool isRtRestirDebug =
        IsRtRestirDebugView(RtRestirDebugView);
    
    bool bypassAccum = 
        (RtAccumulate == 0) ||
        isRtShadingDebug ||
        isMotionDebug ||
        isViewZDebug ||
        isSurfaceIdDebug ||
        isDiffuseAlbedoDebug ||
        isRtRestirInitialDebug ||
        isRtRestirResolveDispatch ||
        isRtRestirDebug;
    
    g_AovViewZRaw[pixel] = RT_VIEWZ_INVALID;
    g_AovSurfaceId[pixel] = RT_SURFACE_ID_INVALID;
    g_AovDiffuseAlbedo[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    
    uint rng = InitRng(pixel, RtSampleIndex, RtResetId);
    if (samplingDebugView == 96u)
    {
        // Alias/PDF heatmap. Horizontal pixel coordinate scans the alias table.
        if (RtEnvAliasCount == 0u)
        {
            g_Output[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
            return;
        }

        uint idx = min(
            uint((float(pixel.x) / max(1.0f, float(dim.x))) * float(RtEnvAliasCount)),
            RtEnvAliasCount - 1u);

        RtEnvAliasEntry e = g_EnvAlias[idx];

        float pdfVis = saturate(log2(1.0f + e.pdf) / 8.0f);
        float qVis = saturate(e.q);

        g_Output[pixel] = float4(RtHeat(pdfVis).rg, qVis, 1.0f);
        return;
    }

    if (samplingDebugView == 97u)
    {
        // Sampled env direction/pdf.
        RtEnvRandoms envRand = MakeRtEnvRandoms(97u, 0u);
        EnvSample env = SampleEnvironment(envRand.uEnvSelect, envRand.uEnvTexel);

        if (!env.valid)
        {
            g_Output[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
            return;
        }

        float pdfVis = saturate(log2(1.0f + env.pdf) / 8.0f);
        float3 dirVis = env.wi * 0.5f + 0.5f;

        g_Output[pixel] = float4(lerp(dirVis, RtHeat(pdfVis), 0.35f), 1.0f);
        return;
    }

    if (samplingDebugView == 103u)
    {
        g_Output[pixel] = float4(RtSamplingFallbackMask(), 1.0f);
        return;
    }

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
    ray.TMin = 0.001f;
    ray.TMax = 10000.0f;

    float3 oldDiff =
        (!bypassAccum && RtSampleIndex != 0) ? g_AccumDiff[pixel].rgb : 0.0f.xxx;

    float3 oldSpec =
        (!bypassAccum && RtSampleIndex != 0) ? g_AccumSpec[pixel].rgb : 0.0f.xxx;

    RayPayload diffPayload;
    diffPayload.color = 0.0f.xxx;
    diffPayload.rayType = RT_RAY_PRIMARY_DIFFUSE;
    diffPayload.occluded = 0u;
    diffPayload.rng = diffRng;

    TraceRay(
        g_Scene,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF,
        0,
        1,
        0,
        ray,
        diffPayload);

    // ReSTIR resolve mode is authored by closest-hit / miss directly into
    // g_RestirResolvedDiffuse, g_RestirResolvedSpec, and optional debug output.
    // Do not let normal RayGen beauty/accumulation overwrite it.
    if (RtRestirDispatchMode == 1u)
    {
        return;
    }

    // 104/105 must run after the primary trace because closest-hit writes
    // g_RestirInitialReservoir.
    if (isRtRestirInitialDebug)
    {
        RtRestirReservoir rr = g_RestirInitialReservoir[pixelIndex];

        if (RtRestirDebugView == 104u)
        {
            if (!ReservoirValid(rr))
            {
                g_Output[pixel] = float4(0.0f, 0.0f, 0.05f, 1.0f);
                return;
            }

            float targetLum = ReservoirTarget(rr);

            float v = saturate(log2(1.0f + targetLum * 10.0f) / 10.0f);

            g_Output[pixel] = float4(RtHeat(v), 1.0f);
            return;
        }

        if (RtRestirDebugView == 105u)
        {
            if (!ReservoirValid(rr))
            {
                g_Output[pixel] = float4(0.0f, 0.0f, 0.05f, 1.0f);
                return;
            }

            float sourcePdf = ReservoirSourcePdf(rr);

            // Stronger scale for bring-up. The original log2(1 + pdf) / 8
            // can look nearly all blue if pdf values are small.
            float v = saturate(log2(1.0f + sourcePdf * 1000.0f) / 12.0f);

            g_Output[pixel] = float4(RtHeat(v), 1.0f);
            return;
        }
    }

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
    
    if (DebugView == 51)
    {
        float2 prevUV = g_AovMotion[pixel];
        float2 currUV = (float2(pixel) + 0.5f) / float2(dim);
        bool validPrev = PrevUVValid(prevUV);

        // Visualize motion as prevUV - currUV centered around 0.5.
        float2 motion = validPrev ? (prevUV - currUV) : 0.0f.xx;
        float2 vis = saturate(motion * 8.0f + 0.5f);

        g_Output[pixel] = float4(vis, validPrev ? 0.0f : 1.0f, 1.0f);
        return;
    }

    if (DebugView == 52)
    {
        float2 prevUV = g_AovMotion[pixel];
        bool invalidPrev = !PrevUVValid(prevUV);

        float v = invalidPrev ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }

    if (DebugView == 53)
    {
        float2 prevUV = g_AovMotion[pixel];
        bool invalidPrev = !PrevUVValid(prevUV);

        float v = invalidPrev ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }

    if (DebugView == 61)
    {
        float viewZ = g_AovViewZRaw[pixel];
        bool validViewZ = ViewZValid(viewZ);

        // Conservative bring-up visualization range. Promote to a knob later only if needed.
        float v = validViewZ ? saturate(viewZ / RT_VIEWZ_VIS_MAX) : 0.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }

    if (DebugView == 62)
    {
        float viewZ = g_AovViewZRaw[pixel];
        float v = ViewZValid(viewZ) ? 0.0f : 1.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 65)
    {
        uint surfaceId = g_AovSurfaceId[pixel];
        float v = SurfaceIdToGray(surfaceId);

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }

    if (DebugView == 66)
    {
        uint surfaceId = g_AovSurfaceId[pixel];
        float v = SurfaceIdValid(surfaceId) ? 0.0f : 1.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 67)
    {
        float3 albedo = saturate(g_AovDiffuseAlbedo[pixel].rgb);
        g_Output[pixel] = float4(LinearToSRGB(albedo), 1.0f);
        return;
    }

    if (DebugView == 68)
    {
        float4 albedoSample = g_AovDiffuseAlbedo[pixel];
        float v = albedoSample.a > 0.5f ? 0.0f : 1.0f;

        g_Output[pixel] = float4(v.xxx, 1.0f);
        return;
    }
    
    if (DebugView == 74)
    {
        uint surfaceId = g_AovSurfaceId[pixel];
        g_Output[pixel] = float4(SurfaceIdToColor(surfaceId), 1.0f);
        return;
    }

    if (DebugView == 75)
    {
        uint surfaceId = g_AovSurfaceId[pixel];
        float v = SurfaceIdValid(surfaceId) ? 0.0f : 1.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
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
    payload.occluded = 0u;
    
    if (RtRestirDispatchMode == 1u &&
    payload.rayType == RT_RAY_PRIMARY_DIFFUSE)
    {
        uint2 pixel = DispatchRaysIndex().xy;

        g_RestirResolvedDiffuse[pixel] = float4(0.0f.xxx, 1.0f);
        g_RestirResolvedSpec[pixel] = float4(0.0f.xxx, 1.0f);

        if (RtRestirDebugView == 110u ||
            RtRestirDebugView == 111u ||
            RtRestirDebugView == 112u ||
            RtRestirDebugView == 113u)
        {
            g_Output[pixel] = float4(0.0f.xxx, 1.0f);
        }
        else if (RtRestirDebugView == 114u)
        {
            // B = no current surface / primary miss.
            g_Output[pixel] = float4(0.0f, 0.0f, 1.0f, 1.0f);
        }

        payload.color = 0.0f.xxx;
        return;
    }
    
    if (payload.rayType == RT_RAY_SHADOW)
    {
        return;
    }
        
    if (payload.rayType == RT_RAY_INDIRECT)
    {
        uint2 pixel = DispatchRaysIndex().xy;
        g_AovViewZRaw[pixel] = RT_VIEWZ_INVALID;

        payload.color = 0.0f.xxx;
        return;
    }
    
    if (payload.rayType == RT_RAY_PRIMARY_SPECULAR)
    {
        uint2 pixel = DispatchRaysIndex().xy;
        g_AovViewZRaw[pixel] = RT_VIEWZ_INVALID;

        const bool useSpecDisplaySky =
            RtSky.enabled != 0u &&
            RtSky.visibleInDxr != 0u &&
            RtSky.specularMissUsesDisplaySky != 0u;

        payload.color =
            useSpecDisplaySky
                ? SampleRtDisplaySky(WorldRayDirection())
                : 0.0f.xxx;

        return;
    }
    
    const float3 rayDir =
        WorldRayDirection();

    const bool useDisplaySky =
        RtSky.enabled != 0u &&
        RtSky.visibleInDxr != 0u;

    float3 sky =
        useDisplaySky
            ? SampleRtDisplaySky(rayDir)
            : EvalSky(rayDir);
   
    
    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE)
    {
        uint2 pixel = DispatchRaysIndex().xy;
       
        g_AovNormal[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_AovDepth[pixel] = 1.0f;
        g_AovMotion[pixel] = float2(-1.0f, -1.0f);
        g_AovViewZRaw[pixel] = RT_VIEWZ_INVALID;
        g_AovSurfaceId[pixel] = RT_SURFACE_ID_INVALID;
        g_AovDiffuseAlbedo[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        
        payload.color = sky;
        return;
    }

    payload.color = sky;
}

[shader("anyhit")]
void AnyHit(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attr)
{
    RTInstanceData data =
        g_InstanceData[InstanceID()];

    if (data.alphaMode != ALPHA_MODE_MASK)
        return;

    SurfaceUvsRT uvs =
        FetchSurfaceUvs(
            data,
            PrimitiveIndex(),
            attr.barycentrics);

    float2 baseUv =
        SelectRtUv(
            data.baseColorTexCoord,
            uvs.uv0,
            uvs.uv1);

    float alpha =
        SampleBaseColorTex(data.materialId, baseUv).a *
        data.baseColorFactor.a;

    if (alpha < data.emissiveFactorAndAlphaCutoff.a)
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (payload.rayType == RT_RAY_PRIMARY_SPECULAR)
    {
        uint2 pixel = DispatchRaysIndex().xy;
        // Linear primary visible-surface distance in world units.
        // Current implementation stores RayT along the primary camera ray.
        // This is a ViewZ-compatible primary distance guide,
        // not strict camera-space Z and not specular secondary-ray hit distance.
        g_AovViewZRaw[pixel] = RayTCurrent();
    }
    
    if (payload.rayType == RT_RAY_SHADOW)
    {
        payload.occluded = 1u;
        return;
    }
    
    // For radiance rays, occluded is reused as a geometry-hit flag.
    payload.occluded = 1u;

    const uint instanceID = InstanceID();
    const uint prim = PrimitiveIndex();

    float2 bary = float2(attr.barycentrics.x, attr.barycentrics.y);
    
    float3 worldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
        
    SurfaceBasisRT surface = FetchSurfaceBasis(instanceID, prim, bary);
    
    float3 geomNormal = surface.worldGeomNormal;
    // Note: Raster respects glTF doubleSided through per-material no-cull PSOs.
    // DXR currently shades surfaces two-sided for robustness. RTInstanceData.doubleSided
    // is uploaded for parity with raster/material metadata, but exact per-material
    // backface rejection in DXR is deferred because it would require a deliberate
    // any-hit / hit-group policy for opaque non-masked geometry too.
    if (dot(geomNormal, WorldRayDirection()) > 0.0f)
        geomNormal = -geomNormal;

    RTInstanceData data = g_InstanceData[instanceID];

    float2 baseUv =
        SelectRtUv(data.baseColorTexCoord, surface.uv0, surface.uv1);

    float2 normalUv =
        SelectRtUv(data.normalTexCoord, surface.uv0, surface.uv1);

    float2 mrUv =
        SelectRtUv(data.metalRoughTexCoord, surface.uv0, surface.uv1);

    float2 aoUv =
        SelectRtUv(data.occlusionTexCoord, surface.uv0, surface.uv1);

    float2 emissiveUv =
        SelectRtUv(data.emissiveTexCoord, surface.uv0, surface.uv1);

    float ao =
        ComputeRtMaterialOcclusion(
            data,
            aoUv);

    uint currentSurfaceId = MakeSurfaceId(data.objectId, data.materialId);

    float3 T =
        SafeNormalize(
            surface.worldTangent -
            geomNormal * dot(surface.worldTangent, geomNormal));

    float3 B =
        SafeNormalize(cross(geomNormal, T)) *
        surface.tangentSign;

    float3 tangentNormal =
        SampleNormalTex(data.materialId, normalUv);

    tangentNormal.xy *= data.normalScale;
    tangentNormal = SafeNormalize(tangentNormal);

    float3 worldNormal =
        SafeNormalize(
            T * tangentNormal.x +
            B * tangentNormal.y +
            geomNormal * tangentNormal.z);

    if (dot(worldNormal, WorldRayDirection()) > 0.0f)
        worldNormal = -worldNormal;

    float4 baseTex =
        SampleBaseColorTex(data.materialId, baseUv);

    float3 metalRoughTex =
        SampleMetalRoughTex(data.materialId, mrUv);

    float3 emissive =
        ComputeRtEmissive(data, emissiveUv);

    float3 base =
        data.baseColorFactor.rgb *
        baseTex.rgb;

    float roughness =
        saturate(data.roughness * metalRoughTex.g);

    float metallic =
        saturate(data.metallic * metalRoughTex.b);
    
    roughness = max(0.045f, roughness);
    
    // Use outgoing direction for both primary and indirect hits.
    float3 V = SafeNormalize(-WorldRayDirection());
    float3 L = SafeNormalize(-LightDir);
    
    float4 clip = mul(float4(worldPos, 1.0f), ViewProj);
    float depth01 = saturate(clip.z / clip.w);
    
    float3 diffuseAlbedo = base * (1.0f - metallic);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
    
    float pSpec = clamp(Max3(F0), 0.05f, 0.95f);
    float pSpecSafe = saturate(pSpec);
    float pDiffuseSafe = saturate(1.0f - pSpecSafe);
    
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
        
        float2 prevUV = float2(-1.0f, -1.0f);

        if (RtHasPrevMotion != 0u)
        {
            float3 prevWorldPos = TransformPrevObjectToWorld(data, surface.objPos);
            prevUV = ProjectPrevUVFromWorld(prevWorldPos);
        }

        g_AovNormal[pixel] = float4(geomNormal * 0.5f + 0.5f, roughness);
        g_AovDepth[pixel] = depth01;
        g_AovMotion[pixel] = prevUV;
        g_AovSurfaceId[pixel] = currentSurfaceId;
        
        // Linear primary visible-surface distance in world units.
        // Current implementation stores RayT along the primary camera ray.
        // Used as a ViewZ-compatible guide for motion dilation/confidence.
        // This is not strict camera-space Z and not specular secondary-ray hit distance.
        g_AovViewZRaw[pixel] = RayTCurrent();
        
        float3 diffuseAlbedo = saturate(base * (1.0f - saturate(metallic)));
        float stableAlbedo = DiffuseAlbedoStable(diffuseAlbedo) ? 1.0f : 0.0f;

        g_AovDiffuseAlbedo[pixel] = float4(diffuseAlbedo, stableAlbedo);
    }
    
    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE &&
    RestirInitialEnabled())
    {
        uint2 pixel = DispatchRaysIndex().xy;
        uint2 dim = DispatchRaysDimensions().xy;
        uint pixelIndex = pixel.y * dim.x + pixel.x;

        RtRestirReservoir r;
        ReservoirClear(r);

        uint rrng = InitRtEnvRng(pixel, 107u, payload.rayType);

        uint candidateCount = max(1u, RtRestirInitialCandidateCount);

    [loop]
        for (uint i = 0u; i < candidateCount; ++i)
        {
            RtEnvRandoms envRand = MakeRtEnvRandoms(107u, i);

            EnvSample env = SampleEnvironment(
                envRand.uEnvSelect,
                envRand.uEnvTexel);

            if (!env.valid)
                continue;            

            float NoL = saturate(dot(worldNormal, env.wi));
            if (NoL <= 1e-4f || dot(geomNormal, env.wi) <= 0.0f)
                continue;          

            PbrSplit brdf = EvalEnvBrdfSplit(
                base,
                metallic,
                roughness,
                worldNormal,
                V,
                env.wi);

            float3 unoccluded =
                env.Li *
                (brdf.diffuse + brdf.spec) *
                NoL;

            float targetLum = RtDebugLuminance(unoccluded);

            if (targetLum <= RtRestirMinTarget)
                continue;
            
            RtRestirReservoir candidate =
                MakeRestirCandidate(
                    env.wi,
                    env.Li,
                    env.pdf,
                    targetLum,
                    env.index,
                    currentSurfaceId);          

            float candidateWeight =
                ReservoirCandidateWeight(
                    targetLum,
                    env.pdf);

            ReservoirUpdate(
                r,
                candidate,
                candidateWeight,
                rrng);
        }

        ReservoirFinalize(
            r,
            RtRestirMaxM,
            RtRestirMaxWeight);

        g_RestirInitialReservoir[pixelIndex] = r;
    }
    
    if (payload.rayType == RT_RAY_PRIMARY_DIFFUSE &&
    RtRestirDispatchMode == 1u)
    {
        uint2 pixel = DispatchRaysIndex().xy;
        uint2 dim = DispatchRaysDimensions().xy;
        uint pixelIndex = pixel.y * dim.x + pixel.x;

        RtRestirReservoir rr = g_RestirResolveReservoir[pixelIndex];

        float3 outDiffuse = 0.0f.xxx;
        float3 outSpec = 0.0f.xxx;

        float debugW = 0.0f;
        float debugVisibility = 0.0f;
        float debugDiffuseLum = 0.0f;
        float debugSpecLum = 0.0f;

        float3 invalidReason = 0.0f.xxx;

        if (!ReservoirValid(rr))
        {
            invalidReason.r = 1.0f;
        }
        else if (rr.surfaceId != currentSurfaceId)
        {
            invalidReason.g = 1.0f;
        }
        else
        {
            float3 wi = SafeNormalize(rr.sampleDir_pdf.xyz);
            float3 Li = max(rr.sampleLi_target.xyz, 0.0f.xxx);

            float NoL = saturate(dot(worldNormal, wi));

            bool frontFacing =
            NoL > 1e-4f &&
            dot(geomNormal, wi) > 0.0f;

            bool visible = false;

            if (frontFacing)
            {
                visible = TraceEnvironmentVisibility(
                worldPos,
                geomNormal,
                wi);
            }

            if (!frontFacing || !visible)
            {
                invalidReason.b = 1.0f;
            }

            debugVisibility = visible ? 1.0f : 0.0f;

            if (visible)
            {
                PbrSplit brdf = EvalEnvBrdfSplit(
                base,
                metallic,
                roughness,
                worldNormal,
                V,
                wi);

                float W =
                min(
                    RtRestirMaxWeight,
                    max(0.0f, rr.weightSum_M_W.z));

                debugW = W;

                outDiffuse =
                Li *
                brdf.diffuse *
                NoL *
                W;

                outSpec =
                Li *
                brdf.spec *
                NoL *
                W;

                debugDiffuseLum = RtDebugLuminance(outDiffuse);
                debugSpecLum = RtDebugLuminance(outSpec);
            }
        }

        g_RestirResolvedDiffuse[pixel] = float4(outDiffuse, 1.0f);
        g_RestirResolvedSpec[pixel] = float4(outSpec, 1.0f);

        if (RtRestirDebugView == 110u)
        {
            float v = saturate(debugW / max(1.0f, RtRestirMaxWeight));
            g_Output[pixel] = float4(RtHeat(v), 1.0f);
        }
        else if (RtRestirDebugView == 111u)
        {
            g_Output[pixel] = float4(debugVisibility.xxx, 1.0f);
        }
        else if (RtRestirDebugView == 112u)
        {
            float v = saturate(log2(1.0f + debugDiffuseLum) / 8.0f);
            g_Output[pixel] = float4(RtHeat(v), 1.0f);
        }
        else if (RtRestirDebugView == 113u)
        {
            float v = saturate(log2(1.0f + debugSpecLum) / 8.0f);
            g_Output[pixel] = float4(RtHeat(v), 1.0f);
        }
        else if (RtRestirDebugView == 114u)
        {
            g_Output[pixel] = float4(saturate(invalidReason), 1.0f);
        }

        payload.color = 0.0f.xxx;
        return;
    }
    
    if (payload.rayType == RT_RAY_INDIRECT)
    {
        float sunVisibility =
        TraceShadowVisibility(
            worldPos,
            geomNormal,
            L);
        
        float3 direct = EvalDirectAtSurface(
            base,
            metallic,
            roughness,
            worldNormal,
            V,
            L,
            sunVisibility);

        direct += EvalPointLightsAtSurface(
            base,
            metallic,
            roughness,
            worldNormal,
            V,
            worldPos);

        payload.color = direct;
        return;
    }

    float3 envNeeDiffuse = 0.0f.xxx;
    float3 envNeeSpec = 0.0f.xxx;
    
    uint samplingDebugView = RtSamplingDebugView;

    bool useEnvNeeForFinal =
        RtUseEnvNeeForFinal != 0u &&
        EnvNeeEnabled();

    bool useEnvNeeForDebug =
        IsRtEnvEstimatorDebugView(samplingDebugView) &&
        EnvNeeDebugReady();

    bool shouldComputeEnvNee =
        useEnvNeeForFinal ||
        useEnvNeeForDebug;

    if (shouldComputeEnvNee)
    {
        RtEnvRandoms envRand = MakeRtEnvRandoms(
            payload.rayType,
            0u);

        EnvSample env = SampleEnvironment(
            envRand.uEnvSelect,
            envRand.uEnvTexel);

        if (env.valid)
        {
            float NdotEnv = saturate(dot(worldNormal, env.wi));
            float GdotEnv = dot(geomNormal, env.wi);

            if (NdotEnv > 1e-4f && GdotEnv > 0.0f)
            {
                bool visible = TraceEnvironmentVisibility(
                    worldPos,
                    geomNormal,
                    env.wi);

                if (visible)
                {
                    bool allowSpecEnvNee =
                        roughness >= RtEnvDeltaRoughnessCutoff;
                    
                    PbrSplit envBrdf = EvalEnvBrdfSplit(
                        base,
                        metallic,
                        roughness,
                        worldNormal,
                        V,
                        env.wi);

                    float pdfDiffuse =
                    pDiffuseSafe *
                    PdfDiffuseBrdf(
                        worldNormal,
                        env.wi);

                    float pdfSpec = allowSpecEnvNee
                    ? pSpecSafe *
                        PdfSpecularGGXBrdf(
                            roughness,
                            worldNormal,
                            V,
                            env.wi)
                    : 0.0f;

                    float wDiffuse = RtUseEnvMIS != 0u
                    ? PowerHeuristicN(env.pdf, pdfDiffuse, RtEnvMISPower)
                    : 1.0f;

                    float wSpec = RtUseEnvMIS != 0u && allowSpecEnvNee
                    ? PowerHeuristicN(env.pdf, pdfSpec, RtEnvMISPower)
                    : 1.0f;

                    float invPdf = 1.0f / max(RtEnvPdfEpsilon, env.pdf);

                    envNeeDiffuse =
                        env.Li *
                        envBrdf.diffuse *
                        NdotEnv *
                        wDiffuse *
                        invPdf;

                    if (allowSpecEnvNee)
                    {
                        envNeeSpec =
                        env.Li *
                        envBrdf.spec *
                        NdotEnv *
                        wSpec *
                        invPdf;
                    }

                    if (RtEnvNeeFireflyGuard != 0u)
                    {
                        float maxNee = max(1e-4f, RtEnvNeeMaxRadiance);
                        envNeeDiffuse = min(envNeeDiffuse, maxNee.xxx);
                        envNeeSpec = min(envNeeSpec, maxNee.xxx);
                    }
                }
            }
        }
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
        payload.color = float3(frac(baseUv), 0.0f);
        return;
    }
    

    float3 indirectDiffuse = 0.0f.xxx;
    float3 indirectSpec = 0.0f.xxx;
    float3 lobeVis = 0.0f.xxx;

    bool allowIndirect =
        ((RtEnableIndirect != 0) || samplingDebugView == 101u) &&
        (
            DebugView == 0 ||
            DebugView == 9 ||
            DebugView == 10 ||
            DebugView == 11 ||
            DebugView == 48 ||
            DebugView == 49 ||
            DebugView == 50 ||
            samplingDebugView == 101u
        );

    if (allowIndirect)
    {
        //float3 diffuseAlbedo = base * (1.0f - metallic);
        //float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), base, metallic);
        //float pSpec = clamp(Max3(F0), 0.05f, 0.95f);

        float xi = Rand01(payload.rng);
        bool chooseSpec = (xi < pSpecSafe);

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

                bool brdfRayMissedEnvironment = (bounce.occluded == 0u);

                float envMisWeight = 1.0f;

                if (brdfRayMissedEnvironment)
                {
                    if (!EnvBrdfEnvironmentContributionEnabled(true, roughness))
                    {
                        bounce.color = 0.0f.xxx;
                    }
                    else if (EnvBrdfEnvironmentMisEnabled(true, roughness, samplingDebugView))
                    {
                        float pdfBrdf =
                        pSpecSafe *
                        PdfSpecularGGXBrdf(
                            roughness,
                            worldNormal,
                            V,
                            wi);

                        float pdfEnv = PdfEnvironment(wi);

                        envMisWeight =
                            PowerHeuristicN(
                                pdfBrdf,
                                pdfEnv,
                                RtEnvMISPower);
                    }
                }

                float G = G_Smith(NdotV, NdotL, roughness);
                float3 F = F_Schlick(VdotH, F0);

                // Simplified GGX importance-sampling estimator.
                float3 specWeight =
                    (G * F * VdotH) / max(1e-4f, NdotV * NdotH);

                // Firefly control.
                specWeight = min(specWeight, float3(10.0f, 10.0f, 10.0f));
                float3 bounceRadiance = min(bounce.color, float3(20.0f, 20.0f, 20.0f));

                indirectSpec =
                    bounceRadiance *
                    specWeight *
                    envMisWeight /
                    max(1e-4f, pSpecSafe);
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

                bool brdfRayMissedEnvironment = (bounce.occluded == 0u);

                float envMisWeight = 1.0f;

                if (brdfRayMissedEnvironment)
                {
                    if (!EnvBrdfEnvironmentContributionEnabled(false, roughness))
                    {
                        bounce.color = 0.0f.xxx;
                    }
                    else if (EnvBrdfEnvironmentMisEnabled(false, roughness, samplingDebugView))
                    {
                        float pdfBrdf =
                            pDiffuseSafe *
                            PdfDiffuseBrdf(
                                worldNormal,
                                wi);

                        float pdfEnv = PdfEnvironment(wi);

                        envMisWeight =
                            PowerHeuristicN(
                                pdfBrdf,
                                pdfEnv,
                                RtEnvMISPower);
                    }
                }

                indirectDiffuse =
                    bounce.color *
                    diffuseAlbedo *
                    envMisWeight /
                    max(1e-4f, pDiffuseSafe);

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
    
    if (IsRtEnvEstimatorDebugView(samplingDebugView))
    {
        RtEnvRandoms envRandDbg = MakeRtEnvRandoms(98u, 0u);
        EnvSample envDbg = SampleEnvironment(
            envRandDbg.uEnvSelect,
            envRandDbg.uEnvTexel);

        float3 dbg = 0.0f.xxx;

        if (samplingDebugView == 98u)
        {
            // MIS weight heatmap for the env NEE sample.
            if (envDbg.valid)
            {
                bool allowSpecEnvNee =
                    roughness >= RtEnvDeltaRoughnessCutoff;

                float pdfDiffuse =
                    pDiffuseSafe *
                    PdfDiffuseBrdf(
                        worldNormal,
                        envDbg.wi);

                float pdfSpec = allowSpecEnvNee
                    ? pSpecSafe *
                        PdfSpecularGGXBrdf(
                            roughness,
                            worldNormal,
                            V,
                            envDbg.wi)
                    : 0.0f;

                float wDiffuse = RtUseEnvMIS != 0u
                    ? PowerHeuristicN(envDbg.pdf, pdfDiffuse, RtEnvMISPower)
                    : 1.0f;

                float wSpec = RtUseEnvMIS != 0u && allowSpecEnvNee
                    ? PowerHeuristicN(envDbg.pdf, pdfSpec, RtEnvMISPower)
                    : 0.0f;

                dbg = float3(wDiffuse, wSpec, 0.0f);
            }

            payload.color = dbg;
            return;
        }

        if (samplingDebugView == 99u)
        {
            // Env visibility mask.
            if (envDbg.valid)
            {
                float NoL = saturate(dot(worldNormal, envDbg.wi));
                bool visible =
                NoL > 1e-4f &&
                dot(geomNormal, envDbg.wi) > 0.0f &&
                TraceEnvironmentVisibility(worldPos, geomNormal, envDbg.wi);

                dbg = visible ? 1.0f.xxx : 0.0f.xxx;
            }

            payload.color = dbg;
            return;
        }

        if (samplingDebugView == 100u)
        {
        // Direct env NEE luminance.
            float lum = RtDebugLuminance(envNeeDiffuse + envNeeSpec);
            payload.color = RtHeat(saturate(lum * 0.1f));
            return;
        }

        if (samplingDebugView == 101u)
        {
        // BRDF env-hit luminance approximation.
            float lum = RtDebugLuminance(indirectDiffuse + indirectSpec);
            payload.color = RtHeat(saturate(lum * 0.1f));
            return;
        }

        if (samplingDebugView == 102u)
        {
        // Technique selection / mode.
        // R = BRDF-only, G = env-only, B = MIS reference.
            uint mode = EffectiveEnvSamplingMode();

            if (mode == RT_ENV_SAMPLING_BRDF_ONLY)
                dbg = float3(1.0f, 0.0f, 0.0f);
            else if (mode == RT_ENV_SAMPLING_ENV_ONLY)
                dbg = float3(0.0f, 1.0f, 0.0f);
            else
                dbg = float3(0.0f, 0.25f + 0.75f * envRandDbg.uTechnique, 1.0f);

            payload.color = dbg;
            return;
        }
    }
    
    PbrSplit directSplit = EvalDirectPbrSplit(
        base,
        metallic,
        roughness,
        worldNormal,
        V,
        L);

    // Apply sun visibility to the sun only.
    directSplit.diffuse *= shadowVisibility;
    directSplit.spec *= shadowVisibility;

    // Add unshadowed local point lights.
    PbrSplit pointSplit = EvalPointLightsPbrSplitAtSurface(
        base,
        metallic,
        roughness,
        worldNormal,
        V,
        worldPos);

    directSplit.diffuse += pointSplit.diffuse;
    directSplit.spec += pointSplit.spec;

    float3 ambient =
        base * 0.03f * ao;

    float3 envDiffuseTerm =
        useEnvNeeForFinal
            ? envNeeDiffuse
            : iblDiffuse;

    float3 envSpecTerm =
        useEnvNeeForFinal
            ? envNeeSpec
            : iblSpecular;

    float3 sampleDiffuse =
        ambient +
        directSplit.diffuse +
        envDiffuseTerm * ao +
        RtIndirectScale * indirectDiffuse +
        emissive;

    float3 sampleSpec =
        directSplit.spec +
        envSpecTerm +
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

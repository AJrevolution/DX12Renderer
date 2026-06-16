#include "Common.hlsli"
#include "RtReservoir.hlsli"

// RT DebugView ownership for this pass:
//   106 = ReSTIR temporal reuse mask
//   107 = ReSTIR temporal reservoir M / confidence

StructuredBuffer<RtRestirReservoir> g_CurrInitialReservoir : register(t0);
StructuredBuffer<RtRestirReservoir> g_PrevTemporalReservoir : register(t1);

Texture2D<float4> g_CurrNormal : register(t2);
Texture2D<float> g_CurrDepth : register(t3);
Texture2D<float2> g_CurrPrevUV : register(t4);
Texture2D<float> g_CurrViewZ : register(t5);
Texture2D<uint> g_CurrSurfaceId : register(t6);

Texture2D<float4> g_PrevNormal : register(t7);
Texture2D<float> g_PrevDepth : register(t8);
Texture2D<float> g_PrevViewZ : register(t9);
Texture2D<uint> g_PrevSurfaceId : register(t10);

RWStructuredBuffer<RtRestirReservoir> g_OutTemporalReservoir : register(u0);
RWTexture2D<float4> g_Output : register(u1);

SamplerState g_LinearClamp : register(s0);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

cbuffer RtRestirTemporalConstants : register(b0)
{
    float2 InvResolution;
    uint TemporalEnabled;
    uint HistoryValid;

    uint SurfaceIdHistoryValid;
    uint ViewZHistoryValid;
    uint DebugView;
    uint FrameIndex;

    float DepthSigma;
    float NormalSigma;
    float RoughnessSigma;
    float ViewZSigma;

    float ReprojectMinWeight;
    float MaxM;
    float MaxAge;
    float MaxWeight;
};

uint HashUintRtRestir(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float3 UnpackNormal(float4 packed)
{
    return SafeNormalize(packed.xyz * 2.0f - 1.0f);
}

bool PrevUVValid(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float GuideWeight(
    uint2 currPixel,
    uint2 prevPixel,
    out float reuseWeight)
{
    reuseWeight = 0.0f;

    // If these flags are false, the C++ side did not prove that the previous
    // guide histories are real previous-frame histories. Do not reuse.
    if (SurfaceIdHistoryValid == 0u || ViewZHistoryValid == 0u)
        return 0.0f;

    uint currId = g_CurrSurfaceId[currPixel];
    uint prevId = g_PrevSurfaceId[prevPixel];

    if (!SurfaceIdValid(currId) ||
        !SurfaceIdValid(prevId) ||
        currId != prevId)
    {
        return 0.0f;
    }

    float currDepth = g_CurrDepth[currPixel];
    float prevDepth = g_PrevDepth[prevPixel];

    if (currDepth >= 0.9999f || prevDepth >= 0.9999f)
        return 0.0f;

    float currZ = g_CurrViewZ[currPixel];
    float prevZ = g_PrevViewZ[prevPixel];

    if (!DistanceValid(currZ) || !DistanceValid(prevZ))
        return 0.0f;

    float4 currNR = g_CurrNormal[currPixel];
    float4 prevNR = g_PrevNormal[prevPixel];

    float3 currN = UnpackNormal(currNR);
    float3 prevN = UnpackNormal(prevNR);
    
    float normalDot = dot(currN, prevN);

    if (normalDot < 0.95f)
        return 0.0f;

    float currR = currNR.a;
    float prevR = prevNR.a;

    float wDepth =
        exp(-abs(currDepth - prevDepth) / max(1e-5f, DepthSigma));

    float wNormal =
        exp(-(1.0f - saturate(dot(currN, prevN))) / max(1e-5f, NormalSigma));

    float wRough =
        exp(-abs(currR - prevR) / max(1e-5f, RoughnessSigma));

    float wViewZ =
        exp(-abs(currZ - prevZ) / max(1e-5f, ViewZSigma));

    reuseWeight = wDepth * wNormal * wRough * wViewZ;
    return reuseWeight;
}

bool PreviousReservoirDirectionCompatible(
    RtRestirReservoir prev,
    uint2 currPixel,
    uint2 prevPixel)
{
    float3 currN = UnpackNormal(g_CurrNormal[currPixel]);
    float3 prevN = UnpackNormal(g_PrevNormal[prevPixel]);

    float3 wi = SafeNormalize(prev.sampleDir_pdf.xyz);

    float currNoL = saturate(dot(currN, wi));
    float prevNoL = saturate(dot(prevN, wi));

    if (currNoL <= 1e-4f || prevNoL <= 1e-4f)
        return false;

    float ratio =
        min(currNoL, prevNoL) /
        max(1e-4f, max(currNoL, prevNoL));

    return ratio >= 0.50f;
}

float3 Heat(float v)
{
    v = saturate(v);

    return saturate(float3(
        smoothstep(0.35f, 1.00f, v),
        smoothstep(0.10f, 0.80f, v) * (1.0f - smoothstep(0.85f, 1.00f, v)),
        1.0f - smoothstep(0.00f, 0.65f, v)));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);

    if (pixel.x >= width || pixel.y >= height)
        return;

    uint pixelIndex = pixel.y * width + pixel.x;

    RtRestirReservoir curr = g_CurrInitialReservoir[pixelIndex];

    RtRestirReservoir outR;
    ReservoirClear(outR);

    uint rng =
        HashUintRtRestir(
            pixel.x * 1973u ^
            pixel.y * 9277u ^
            FrameIndex * 26699u ^
            0xB5297A4Du);

    bool usedCurrent = false;
    bool usedPrevious = false;
    float temporalReuseWeight = 0.0f;

    if (ReservoirValid(curr))
    {
        ReservoirUpdateWeighted(
            outR,
            curr,
            max(0.0f, curr.weightSum_M_W.x),
            max(1.0f, curr.weightSum_M_W.y),
            rng);

        usedCurrent = true;
    }

    if (TemporalEnabled != 0u &&
        HistoryValid != 0u &&
        ReservoirValid(curr))
    {
        float2 prevUV = g_CurrPrevUV[pixel];

        if (PrevUVValid(prevUV))
        {
            
            int2 prevPixelI =
                int2(prevUV * float2(width, height));

            prevPixelI =
                clamp(prevPixelI, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            uint2 prevPixel = uint2(prevPixelI);
            uint prevIndex = prevPixel.y * width + prevPixel.x;

            RtRestirReservoir prev = g_PrevTemporalReservoir[prevIndex];

            float guideWeight = 0.0f;
            GuideWeight(pixel, prevPixel, guideWeight);

            if (ReservoirValid(prev) &&
                PreviousReservoirDirectionCompatible(prev, pixel, prevPixel) &&
                guideWeight >= ReprojectMinWeight &&
                float(prev.age) < MaxAge)
            {
                prev.flags |= RT_RESTIR_RESERVOIR_REPROJECTED;
                prev.age = min(prev.age + 1u, 0xFFFFFFFEu);
                prev.weightSum_M_W.w = guideWeight;

                float prevM =
                    max(1.0f, min(prev.weightSum_M_W.y, MaxM));

                float prevCandidateWeight =
                    max(0.0f, prev.weightSum_M_W.x) *
                    guideWeight;

                ReservoirUpdateWeighted(
                    outR,
                    prev,
                    prevCandidateWeight,
                    prevM,
                    rng);

                usedPrevious = true;
                temporalReuseWeight = guideWeight;
            }
        }
    }

    ReservoirFinalize(outR, MaxM, MaxWeight);

    if (!ReservoirValid(outR))
    {
        ReservoirClear(outR);
    }

    g_OutTemporalReservoir[pixelIndex] = outR;

    if (DebugView == 106u)
    {
        float v = usedPrevious ? 1.0f : 0.0f;
        g_Output[pixel] = float4(v.xxx, 1.0f);
    }
    else if (DebugView == 107u)
    {
        float m = ReservoirValid(outR) ? outR.weightSum_M_W.y : 0.0f;
        float conf = ReservoirValid(outR) ? outR.weightSum_M_W.w : 0.0f;

        float mVis = saturate(m / max(1.0f, MaxM));
        g_Output[pixel] = float4(Heat(mVis).rg, saturate(conf), 1.0f);
    }
}

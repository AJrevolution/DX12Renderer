#include "Common.hlsli"

// RT DebugView ownership for this pass:
// Temporal owns:
//   18 = reprojection validity
//   19 = rejection / disocclusion
//   20 = chosen previous UV
//   21 = history-current difference
//   22 = history length
//   23 = temporal alpha
//   24 = moments variance
//   25 = warm-up / history factor
//   26 = depth reprojection error
//   32 = spec-direction reuse mask
//   33 = spec-direction dot
//   34 = reprojection search chosen offset
//   35 = reprojection best score
//   36 = confidence-scaled alpha
//   45 = variance-normalized signal
//   46 = reprojection best score
//   47 = final alpha after variance shaping
//   56 = motion confidence consumed by temporal
//   57 = alpha after motion-confidence scaling
//   58 = post-power motion confidence used for temporal weighting
//   71 = temporal hit-distance weight visible on spec reuse
//   72 = mismatch mask highlights rejected spec history
// Do not include 37..44 here.
// Those are history-select / A-Trous debug IDs.

Texture2D<float4> g_CurrAccum : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<float4> g_PrevAccum : register(t3);
Texture2D<float4> g_PrevNormal : register(t4);
Texture2D<float> g_PrevDepth : register(t5);
Texture2D<float2> g_PrevMoments : register(t6);
Texture2D<float2> g_CurrPrevUV : register(t7);
Texture2D<float> g_CurrMotionConf : register(t8);
Texture2D<float> g_CurrHitDist : register(t9);
Texture2D<float> g_PrevHitDist : register(t10);
Texture2D<float> g_PrevHitDistConf : register(t11);

RWTexture2D<float4> g_HistoryOut : register(u0); // linear
RWTexture2D<float4> g_Output : register(u1); // display
RWTexture2D<float2> g_MomentsOut : register(u2);

SamplerState g_LinearClamp : register(s0);

static const float HIT_DIST_INVALID = -1.0f;

cbuffer RtTemporalConstants : register(b0)
{
    row_major float4x4 CurrInvViewProj;
    row_major float4x4 PrevViewProj;

    float2 InvResolution;
    float TemporalAlpha;
    float DepthSigma;
    
    float NormalSigma;
    float RoughnessSigma;
    float SpecDirSigma;
    float SpecDirRoughCutoff;
    
    uint TemporalEnabled;
    uint HistoryValid;
    uint DebugView;
    uint _pad0;
    
    uint ReprojectRadius;
    float ReprojectMinConf;
    float MotionConfMin;
    float MotionConfPower;
    
    float HitDistSigmaScale;
    float HitDistRoughCutoff;
    float HitDistConfMin;
    float _padHitDist0;
    
    float VarianceScale;
    float VarianceBias;
    float VarianceAlphaBoost;
    uint EnableVarianceBoost;
    
    float4 CurrCameraPos;
    float4 PrevCameraPos;
};

float3 UnpackNormal(float4 packed)
{
    float3 n = packed.xyz * 2.0f - 1.0f;
    return SafeNormalize(n);
}

float3 ReconstructWorldPos(uint2 pixel, float depth01)
{
    float2 uv = (float2(pixel) + 0.5f) * InvResolution;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 clip = float4(ndc, depth01, 1.0f);
    float4 world = mul(clip, CurrInvViewProj);
    return world.xyz / max(1e-6f, world.w);
}

float2 ProjectPrevUV(float3 worldPos)
{
    float4 prevClip = mul(float4(worldPos, 1.0f), PrevViewProj);
    float2 prevNdc = prevClip.xy / max(1e-6f, prevClip.w);
    return float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
}

bool PrevUVValid(float2 uv)
{
    return uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

float2 SelectPrevUV(uint2 pixel, float3 worldPos)
{
    float2 storedPrevUV = g_CurrPrevUV[pixel];

    if (PrevUVValid(storedPrevUV))
        return storedPrevUV;

    return ProjectPrevUV(worldPos);
}

void NeighborhoodMinMax(uint2 pixel, out float3 cmin, out float3 cmax)
{
    uint width, height;
    g_CurrAccum.GetDimensions(width, height);

    cmin = float3(1e20f, 1e20f, 1e20f);
    cmax = float3(-1e20f, -1e20f, -1e20f);

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 p = int2(pixel) + int2(x, y);
            p = clamp(p, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float3 c = g_CurrAccum[uint2(p)].rgb;
            cmin = min(cmin, c);
            cmax = max(cmax, c);
        }
    }
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 ReflectDir(float3 camPos, float3 worldPos, float3 N)
{
    float3 V = SafeNormalize(camPos - worldPos); // surface -> camera
    return SafeNormalize(reflect(-V, N));
}

float3 RoughReflect(float3 R, float3 N, float rough)
{
    float t = rough * rough;
    return SafeNormalize(lerp(R, N, t));
}

float EvalSpecWeight(
    float3 worldPos,
    float3 currNormal,
    float currR,
    float3 prevNormal,
    float prevR,
    out bool specOk,
    out float specDot)
{
    specOk = true;
    specDot = 1.0f;

    float rMin = min(currR, prevR);
    if (rMin >= SpecDirRoughCutoff)
        return 1.0f;

    float3 R0 = ReflectDir(CurrCameraPos.xyz, worldPos, currNormal);
    float3 R1 = ReflectDir(PrevCameraPos.xyz, worldPos, prevNormal);

    float3 RR0 = RoughReflect(R0, currNormal, currR);
    float3 RR1 = RoughReflect(R1, prevNormal, prevR);

    specDot = saturate(dot(RR0, RR1));
    specOk = specDot > (1.0f - SpecDirSigma);

    return saturate((specDot - (1.0f - SpecDirSigma)) / max(1e-4f, SpecDirSigma));
}


bool HitDistValid(float d)
{
    return d >= 0.0f;
}

float EvalHitDistWeight(
    float currHit,
    float prevHit,
    float prevHitConf,
    float currRough,
    float prevRough)
{
    if (HitDistSigmaScale <= 0.0f)
        return 1.0f;

    float roughMin = min(currRough, prevRough);

    if (roughMin >= HitDistRoughCutoff)
        return 1.0f;

    if (!HitDistValid(currHit) || !HitDistValid(prevHit))
        return 1.0f;

    if (prevHitConf < HitDistConfMin)
        return 1.0f;

    float sigmaHit =
        HitDistSigmaScale * max(1e-3f, currHit);

    return exp(-abs(currHit - prevHit) / max(1e-4f, sigmaHit));
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 currColor = g_CurrAccum[pixel].rgb;
    float3 currNormal = UnpackNormal(g_CurrNormal[pixel]);
    float currDepth = g_CurrDepth[pixel];
    float currR = g_CurrNormal[pixel].a;
    float motionConfRaw = saturate(g_CurrMotionConf[pixel]);
    float motionConfW = pow(motionConfRaw, max(MotionConfPower, 1e-3f));
    float motionConfMin = saturate(MotionConfMin);
    
    bool validReuse = false;
    float2 prevUV = 0.0f.xx;
    float3 prevColor = 0.0f.xxx;
    float3 prevNormal = 0.0f.xxx;
    float prevDepth = 1.0f;
    float prevLen = 0.0f;
    float2 prevMoments = 0.0f.xx;
    
    bool specOk = true;
    float specDot = 1.0f;
    
    float bestScore = 0.0f; // Pure reprojection match quality, used by existing debug views.
    float rawBestScore = 0.0f;
    float effectiveScore = 0.0f; // Reprojection score scaled by motion confidence.
    int2 chosenOffset = int2(0, 0);
    
    float varNorm = 0.0f;
    float alphaConfidence = 0.0f;
            
    float bestCandidateBaseScore = 0.0f;
    float bestCandidateHitWeight = 1.0f;

    if (TemporalEnabled != 0 && HistoryValid != 0 && currDepth < 0.9999f)
    {
        float3 worldPos = ReconstructWorldPos(pixel, currDepth);
        prevUV = SelectPrevUV(pixel, worldPos);
        
        uint radius = min(ReprojectRadius, 2u);
        
        bool centerInBounds =
            prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
            prevUV.y >= 0.0f && prevUV.y <= 1.0f;

        if (centerInBounds)
        {
            int2 basePrevPixel = int2(prevUV * float2(width, height));
            basePrevPixel = clamp(basePrevPixel, int2(0, 0), int2(int(width) - 1, int(height) - 1));

            float bestCandidateScore = -1.0f;
            float2 bestPrevUV = prevUV;
            float3 bestPrevColor = 0.0f.xxx;
            float3 bestPrevNormal = 0.0f.xxx;
            float bestPrevDepth = 1.0f;
            float bestPrevLen = 0.0f;
            float2 bestPrevMoments = 0.0f.xx;
            float bestSpecDot = 1.0f;
            bool bestSpecOk = true;
            int2 bestOffset = int2(0, 0);
        
            float normalPow = max(1.0f, 1.0f / max(1e-4f, NormalSigma));
            
            float currHitDist =
                (HitDistSigmaScale > 0.0f)
                ? g_CurrHitDist[pixel]
                : HIT_DIST_INVALID;
        
            [unroll]
            for (int y = -2; y <= 2; ++y)
            {
                [unroll]
                for (int x = -2; x <= 2; ++x)
                {
                    
                    if (abs(x) > int(radius) || abs(y) > int(radius))
                        continue;

             
                    int2 candidatePixel = basePrevPixel + int2(x, y);
                    bool inBounds =
                    candidatePixel.x >= 0 && candidatePixel.x < int(width) &&
                    candidatePixel.y >= 0 && candidatePixel.y < int(height);
                
                    if (!inBounds)
                        continue;
                    uint2 cp = uint2(candidatePixel);
                    float2 candidateUV = (float2(cp) + 0.5f) * InvResolution;
                
                    float4 prevAccumSample = g_PrevAccum.Load(int3(cp, 0));
                    float4 prevNormalPacked = g_PrevNormal.Load(int3(cp, 0));

                    float3 candPrevColor = prevAccumSample.rgb;
                    float candPrevLen = prevAccumSample.a;
                    float3 candPrevNormal = UnpackNormal(prevNormalPacked);
                    float candPrevR = prevNormalPacked.a;
                    float candPrevDepth = g_PrevDepth.Load(int3(cp, 0));
                    float2 candPrevMoments = g_PrevMoments.Load(int3(cp, 0));

                    float depthDelta = abs(currDepth - candPrevDepth);
                    float roughDelta = abs(currR - candPrevR);
                    float nd = saturate(dot(currNormal, candPrevNormal));

                    float wDepth = exp(-depthDelta / max(1e-4f, DepthSigma));
                    float wNormal = pow(nd, normalPow);
                    float wRough = exp(-roughDelta / max(1e-4f, RoughnessSigma));

                    bool candSpecOk = true;
                    float candSpecDot = 1.0f;
                    float wSpec = EvalSpecWeight(
                                                    worldPos,
                                                    currNormal,
                                                    currR,
                                                    candPrevNormal,
                                                    candPrevR,
                                                    candSpecOk,
                                                    candSpecDot);

                    float baseScore = wDepth * wNormal * wRough * wSpec;

                    float candPrevHit = HIT_DIST_INVALID;
                    float candPrevHitConf = 0.0f;

                    if (HitDistSigmaScale > 0.0f)
                    {
                        candPrevHit = g_PrevHitDist.Load(int3(cp, 0));
                        candPrevHitConf = saturate(g_PrevHitDistConf.Load(int3(cp, 0)));
                    }

                    float wHit = EvalHitDistWeight(
                        currHitDist,
                        candPrevHit,
                        candPrevHitConf,
                        currR,
                        candPrevR);

                    float score = baseScore * wHit;

                    if (score > bestCandidateScore)
                    {
                        bestCandidateScore = score;
                        bestPrevUV = candidateUV;
                        bestPrevColor = candPrevColor;
                        bestPrevLen = candPrevLen;
                        bestPrevNormal = candPrevNormal;
                        bestPrevDepth = candPrevDepth;
                        bestPrevMoments = candPrevMoments;
                        bestSpecDot = candSpecDot;
                        bestSpecOk = candSpecOk;
                        bestOffset = int2(x, y);
                        bestCandidateBaseScore = baseScore;
                        bestCandidateHitWeight = wHit;
                    }
                }
            }
        
            rawBestScore = saturate(max(bestCandidateBaseScore, 0.0f));
            bestScore = rawBestScore;
            effectiveScore = saturate(max(bestCandidateScore, 0.0f)) * motionConfW;

            if (bestCandidateScore >= ReprojectMinConf &&
                motionConfRaw >= motionConfMin)
            {
                prevUV = bestPrevUV;
                prevColor = bestPrevColor;
                prevLen = bestPrevLen;
                prevNormal = bestPrevNormal;
                prevDepth = bestPrevDepth;
                prevMoments = bestPrevMoments;
                specDot = bestSpecDot;
                specOk = bestSpecOk;
                chosenOffset = bestOffset;
                validReuse = true;
            }
        }
    }

    float3 currMin, currMax;
    NeighborhoodMinMax(pixel, currMin, currMax);
    float3 prevClamped = clamp(prevColor, currMin, currMax);
    
    if (validReuse)
    {
        float prevVar = max(prevMoments.y - prevMoments.x * prevMoments.x, 0.0f);
        varNorm = saturate(prevVar * VarianceScale + VarianceBias);
    }

    float newLen = 1.0f;

    if (validReuse)
    {
        float inc = effectiveScore * lerp(1.0f, 0.25f, varNorm);
        newLen = min(prevLen + inc, 255.0f);
    }

    float alphaUsed = 0.0f;
    float alphaAfterMotionConf = 0.0f;
    float3 history = currColor;
    
    float currLuma = Luminance(currColor);
    float2 currMoments = float2(currLuma, currLuma * currLuma);
    float2 moments = currMoments;
    
    if (validReuse)
    {
        float k = saturate(newLen / 8.0f);
        alphaUsed = lerp(0.25f, TemporalAlpha, k);
        alphaUsed *= effectiveScore;

        alphaAfterMotionConf = alphaUsed;
        alphaConfidence = alphaUsed;
        
        if (EnableVarianceBoost != 0)
        {
            alphaUsed = saturate(alphaUsed * (1.0f + VarianceAlphaBoost * varNorm));
        }

        history = lerp(currColor, prevClamped, alphaUsed);
        moments = lerp(currMoments, prevMoments, alphaUsed);
    }
    
    float variance = max(moments.y - moments.x * moments.x, 0.0f);
    
    float3 display = history;

    if (DebugView == 18)
    {
        display = validReuse ? 1.0f.xxx : 0.0f.xxx;
    }
    else if (DebugView == 19)
    {
        display = validReuse ? 0.0f.xxx : 1.0f.xxx;
    }
    else if (DebugView == 20)
    {
        display = float3(prevUV, 0.0f);
    }
    else if (DebugView == 21)
    {
        display = saturate(abs(prevClamped - currColor) * 4.0f);
    }
    else if (DebugView == 22)
    {
        display = (newLen / 255.0f).xxx;            // accumulated history length
    }
    else if (DebugView == 23)
    {
        display = alphaUsed.xxx;                    // final blend weight actually used
    }
    else if (DebugView == 24)
    {
        display = saturate(variance * 16.0f).xxx;   // variance / noise estimate
    }
    else if (DebugView == 25)
    {
        float k = saturate(newLen / 8.0f);
        display = k.xxx;                            // history warm-up / convergence factor
    }
    else if (DebugView == 26)
    {
        float reprojErr = 0.0f;

        if (validReuse)
            reprojErr = saturate(abs(currDepth - prevDepth) / max(1e-4f, DepthSigma));
        else
            reprojErr = 1.0f;

        display = reprojErr.xxx;                   // depth mismatch / rejection pressure
    }
    
    else if (DebugView == 32)
    {
        display = specOk ? 1.0f.xxx : 0.0f.xxx;
    }
    else if (DebugView == 33)
    {
        display = specDot.xxx;
    }
    else if (DebugView == 34)
    {
        float r = max(1.0f, float(min(ReprojectRadius, 2u)));
        float2 v = float2(chosenOffset) / r;
        display = float3(v * 0.5f + 0.5f, validReuse ? 1.0f : 0.0f);
    }
    else if (DebugView == 35)
    {
        display = bestScore.xxx;
    }
    else if (DebugView == 36)
    {
        display = alphaConfidence.xxx;
    }
    else if (DebugView == 45)
    {
        display = varNorm.xxx;
    }
    else if (DebugView == 46)
    {
        display = bestScore.xxx;
    }
    else if (DebugView == 47)
    {
        display = alphaUsed.xxx;
    }
    else if (DebugView == 56)
    {
        display = motionConfRaw.xxx;
    }
    else if (DebugView == 57)
    {
        display = alphaAfterMotionConf.xxx;
    }
    else if (DebugView == 58)
    {
        display = motionConfW.xxx;
    }
    
    else if (DebugView == 71)
    {
        display = bestCandidateHitWeight.xxx;
    }
    else if (DebugView == 72)
    {
        float mismatch = (bestCandidateHitWeight < 0.5f) ? 1.0f : 0.0f;
        display = mismatch.xxx;
    }
    
    g_HistoryOut[pixel] = float4(history, newLen);
    g_MomentsOut[pixel] = moments;
    
    bool isTemporalDebug =
        (DebugView >= 18 && DebugView <= 26) ||
        (DebugView >= 32 && DebugView <= 36) ||
        (DebugView >= 45 && DebugView <= 47) ||
        DebugView == 56 ||
        DebugView == 57 ||
        DebugView == 58 ||
        DebugView == 71 ||
        DebugView == 72;

    if (isTemporalDebug)
    {
        g_Output[pixel] = float4(display, 1.0f);
    }
    else
    {
        g_Output[pixel] = float4(LinearToSRGB(history), 1.0f);
    }
}
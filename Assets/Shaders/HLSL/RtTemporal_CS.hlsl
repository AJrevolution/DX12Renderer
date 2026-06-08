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
//   71 = temporal ViewZ similarity weight visible on spec reuse
//   72 = ViewZ mismatch mask highlights rejected spec history
//   76 = SurfaceId match mask
//   77 = SurfaceId invalid mask
//   84 = temporal history color clamp amount
//   85 = temporal moment variance clamp mask
//   87 = temporal signal confidence
//   88 = temporal anti-lag responsiveness amount
//   89 = confidence-shaped history length
//   90 = temporal luminance delta confidence


Texture2D<float4> g_CurrAccum : register(t0);
Texture2D<float4> g_CurrNormal : register(t1);
Texture2D<float> g_CurrDepth : register(t2);
Texture2D<float4> g_PrevAccum : register(t3);
Texture2D<float4> g_PrevNormal : register(t4);
Texture2D<float> g_PrevDepth : register(t5);
Texture2D<float2> g_PrevMoments : register(t6);
Texture2D<float2> g_CurrPrevUV : register(t7);
Texture2D<float> g_CurrMotionConf : register(t8);
Texture2D<float> g_CurrViewZ : register(t9);
Texture2D<float> g_PrevViewZ : register(t10);
Texture2D<float> g_PrevViewZConf : register(t11);
Texture2D<uint> g_CurrSurfaceId : register(t12);
Texture2D<uint> g_PrevSurfaceId : register(t13);

RWTexture2D<float4> g_HistoryOut : register(u0); // linear
RWTexture2D<float4> g_Output : register(u1); // display
RWTexture2D<float2> g_MomentsOut : register(u2);

SamplerState g_LinearClamp : register(s0);

static const uint SURFACE_ID_INVALID = 0xFFFFFFFFu;

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
    
    float ViewZSigmaScale;
    float ViewZRoughCutoff;
    float ViewZConfMin;
    float _padViewZ0;

    float3 DistanceNormParams;
    float DistanceNormSigma;

    uint SurfaceIdHistoryValid;
    uint3 _padSurfaceId0;
    
    float VarianceScale;
    float VarianceBias;
    float VarianceAlphaBoost;
    uint EnableVarianceBoost;
    
    float4 CurrCameraPos;
    float4 PrevCameraPos;
    
    uint EnableRobustMoments;
    float MomentLuminanceMax;
    float MomentVarianceMax;
    float HistoryClampStrength;

    float TemporalNeighborhoodSigmaK;
    float TemporalClampMinWeight;
    float TemporalClampRelaxation;
    float _padRobust0;
    
    uint EnableSignalConfidence;
    float SignalDeltaSigma;
    float ConfidencePower;
    float MinSignalConfidence;

    float AntiLagStrength;
    float VarianceConfidenceScale;
    float HistoryLengthConfidencePower;
    float ResponsiveAlphaBoost;

    float MaxStableHistory;
    float MinStableHistoryForClamp;
    float ConfidenceDebugScale;
    float _padShape0;
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

            float3 c = SanitizeRadiance(g_CurrAccum[uint2(p)].rgb);
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

float EvalViewZWeight(
    float currViewZ,
    float prevViewZ,
    float prevViewZConf,
    float currRough,
    float prevRough)
{
    if (ViewZSigmaScale <= 0.0f)
        return 1.0f;

    float roughMin = min(currRough, prevRough);

    if (roughMin >= ViewZRoughCutoff)
        return 1.0f;

    if (!DistanceValid(currViewZ) || !DistanceValid(prevViewZ))
        return 1.0f;

    if (prevViewZConf < ViewZConfMin)
        return 1.0f;

    float n0 = NormalizeDistance(
        currViewZ,
        currViewZ,
        currRough,
        DistanceNormParams);

    float n1 = NormalizeDistance(
        prevViewZ,
        currViewZ,
        currRough,
        DistanceNormParams);

    float sigma = max(
        1e-4f,
        DistanceNormSigma * max(0.0f, ViewZSigmaScale));

    return DistanceSimilarityWeight(n0, n1, sigma);
}

bool SurfaceIdValid(uint id)
{
    return id != SURFACE_ID_INVALID;
}

float SurfaceIdMatchWeight(uint currId, uint prevId)
{
    if (!SurfaceIdValid(currId) || !SurfaceIdValid(prevId))
        return 0.0f;

    return (currId == prevId) ? 1.0f : 0.0f;
}

struct TemporalNeighborhoodStats
{
    float weight;
    float mean;
    float meanSq;
};

TemporalNeighborhoodStats BuildTemporalStats(uint2 pixel, uint width, uint height)
{
    TemporalNeighborhoodStats s;
    s.weight = 0.0f;
    s.mean = 0.0f;
    s.meanSq = 0.0f;

    float4 centerNR = g_CurrNormal[pixel];
    float3 centerN = UnpackNormal(centerNR);
    float centerR = centerNR.a;
    float centerDepth = g_CurrDepth[pixel];
    uint centerId = g_CurrSurfaceId[pixel];

    if (!SurfaceIdValid(centerId) || centerDepth >= 0.9999f)
        return s;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 qI = int2(pixel) + int2(x, y);
            qI = clamp(qI, int2(0, 0), int2(int(width) - 1, int(height) - 1));
            uint2 q = uint2(qI);

            uint qId = g_CurrSurfaceId[q];
            if (!SurfaceIdValid(qId) || qId != centerId)
                continue;

            float qDepth = g_CurrDepth[q];
            if (qDepth >= 0.9999f)
                continue;

            float4 qNR = g_CurrNormal[q];
            float3 qN = UnpackNormal(qNR);
            float qR = qNR.a;

            float wDepth =
                exp(-abs(centerDepth - qDepth) / max(1e-5f, DepthSigma));

            float wNormal =
                exp(-(1.0f - saturate(dot(centerN, qN))) / max(1e-5f, NormalSigma));

            float wRough =
                exp(-abs(centerR - qR) / max(1e-5f, RoughnessSigma));

            float wViewZ = 1.0f;

            if (ViewZSigmaScale > 0.0f)
            {
                float z0 = g_CurrViewZ[pixel];
                float z1 = g_CurrViewZ[q];

                if (DistanceValid(z0) && DistanceValid(z1))
                {
                    float n0 = NormalizeDistance(
                        z0,
                        z0,
                        centerR,
                        DistanceNormParams);

                    float n1 = NormalizeDistance(
                        z1,
                        z0,
                        centerR,
                        DistanceNormParams);

                    float sigma =
                        max(1e-4f, DistanceNormSigma * max(0.0f, ViewZSigmaScale));

                    wViewZ = DistanceSimilarityWeight(n0, n1, sigma);
                }
            }

            float lum = SafeLuminance(g_CurrAccum[q].rgb);
            float w = wDepth * wNormal * wRough * wViewZ;

            s.mean += lum * w;
            s.meanSq += lum * lum * w;
            s.weight += w;
        }
    }

    if (s.weight > 1e-6f)
    {
        s.mean /= s.weight;
        s.meanSq /= s.weight;
    }

    return s;
}

struct SignalConfidenceResult
{
    float confidence;
    float colorConfidence;
};

SignalConfidenceResult ComputeSignalConfidence(
    float currLum,
    float prevLum,
    float prevVariance,
    float guideScore,
    float motionConf,
    float surfaceIdMatch,
    float viewZWeight,
    float historyClampAmount,
    float momentClampMask)
{
    SignalConfidenceResult r;

    float lumDelta = abs(currLum - prevLum);
    float varianceSigma = sqrt(max(prevVariance, 0.0f));

    float denom =
        max(1e-4f, SignalDeltaSigma * (1.0f + VarianceConfidenceScale * varianceSigma));

    float colorConf = exp(-lumDelta / denom);

    float c =
        colorConf *
        saturate(guideScore) *
        saturate(motionConf) *
        saturate(surfaceIdMatch) *
        saturate(viewZWeight);

    // Robustness feedback from 10.4.
    c *= 1.0f - saturate(historyClampAmount);
    c *= lerp(1.0f, 0.5f, saturate(momentClampMask));

    c = pow(saturate(c), max(1e-4f, ConfidencePower));

    r.confidence = max(MinSignalConfidence, c);
    r.colorConfidence = colorConf;
    return r;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;

    uint width, height;
    g_Output.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float3 currColorRaw = g_CurrAccum[pixel].rgb;
    float3 currColor = SanitizeRadiance(currColorRaw);
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
    float bestCandidateViewZWeight = 1.0f;
    
    float bestSurfaceIdMatch = 1.0f;
    float bestSurfaceIdInvalid = 0.0f;
    
    float momentClampMask = 0.0f;
    float historyClampAmount = 0.0f;

    if (EnableRobustMoments != 0u &&
    (!IsFinite3(currColorRaw) || any(currColorRaw < 0.0f.xxx)))
    {
        momentClampMask = 1.0f;
    }
    
    uint currSurfaceId = g_CurrSurfaceId[pixel];
    
    float signalConfidence = validReuse ? 1.0f : 0.0f;
    float signalColorConfidence = 0.0f;
    float antiLagAlphaMultiplier = 1.0f;
    float shapedHistoryLenDebug = 1.0f;

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
            
            float currViewZ =
                (ViewZSigmaScale > 0.0f)
                ? g_CurrViewZ[pixel]
                : DISTANCE_INVALID;
        
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
                    
                    uint candPrevSurfaceId = SURFACE_ID_INVALID;
                    // Neutral when SurfaceId history is not valid yet.
                    // This prevents SurfaceId warmup from disabling temporal reuse entirely.
                    float wSurfaceId = 1.0f;
                    float surfaceInvalid = 0.0f;

                    if (SurfaceIdHistoryValid != 0u)
                    {
                        candPrevSurfaceId = g_PrevSurfaceId.Load(int3(cp, 0));

                        const bool currIdValid = SurfaceIdValid(currSurfaceId);
                        const bool prevIdValid = SurfaceIdValid(candPrevSurfaceId);

                        if (!currIdValid || !prevIdValid)
                        {
                            wSurfaceId = 0.0f;
                            surfaceInvalid = 1.0f;
                        }
                        else
                        {
                            wSurfaceId = (currSurfaceId == candPrevSurfaceId) ? 1.0f : 0.0f;
                            surfaceInvalid = 0.0f;
                        }
                    }

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

                    float candPrevViewZ = DISTANCE_INVALID;
                    float candPrevViewZConf = 0.0f;

                    if (ViewZSigmaScale > 0.0f)
                    {
                        candPrevViewZ = g_PrevViewZ.Load(int3(cp, 0));
                        candPrevViewZConf = saturate(g_PrevViewZConf.Load(int3(cp, 0)));
                    }

                    float wViewZ = EvalViewZWeight(
                        currViewZ,
                        candPrevViewZ,
                        candPrevViewZConf,
                        currR,
                        candPrevR);

                    float score = baseScore * wViewZ * wSurfaceId;

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
                        bestCandidateViewZWeight = wViewZ;
                        bestSurfaceIdMatch = wSurfaceId;
                        bestSurfaceIdInvalid = surfaceInvalid;
                    }
                }
            }
        
            rawBestScore = saturate(max(bestCandidateBaseScore, 0.0f));
            bestScore = rawBestScore;
            effectiveScore = saturate(max(bestCandidateScore, 0.0f)) * motionConfW;

            const bool surfaceIdAccept =
                SurfaceIdHistoryValid == 0u ||
                bestSurfaceIdMatch > 0.5f;

            if (bestCandidateScore >= ReprojectMinConf &&
                motionConfRaw >= motionConfMin &&
                surfaceIdAccept)
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
    
    if (validReuse)
    {
        prevColor = SanitizeRadiance(prevColor);

        if (EnableRobustMoments != 0u)
        {
            float prevM1 = prevMoments.x;
            float prevM2 = prevMoments.y;

            if (!IsFiniteScalar(prevM1))
            {
                prevM1 = 0.0f;
                momentClampMask = 1.0f;
            }

            prevM1 = max(prevM1, 0.0f);

            if (prevM1 > MomentLuminanceMax)
            {
                prevM1 = MomentLuminanceMax;
                momentClampMask = 1.0f;
            }

            if (!IsFiniteScalar(prevM2))
            {
                prevM2 = prevM1 * prevM1;
                momentClampMask = 1.0f;
            }

            prevM2 = max(prevM2, prevM1 * prevM1);

            if (prevM2 > MomentVarianceMax)
            {
                prevM2 = MomentVarianceMax;
                momentClampMask = 1.0f;
            }

            prevMoments = float2(prevM1, prevM2);
        }
    }

    float3 currMin, currMax;
    NeighborhoodMinMax(pixel, currMin, currMax);
    float3 prevClamped = clamp(prevColor, currMin, currMax);
    
    if (EnableRobustMoments != 0u &&
        validReuse &&
        prevLen >= MinStableHistoryForClamp)
    {
        TemporalNeighborhoodStats stats = BuildTemporalStats(pixel, width, height);

        if (stats.weight >= TemporalClampMinWeight)
        {
            float sigma = RobustSigmaFromMoments(stats.mean, stats.meanSq);

            float sigmaK =
            TemporalNeighborhoodSigmaK *
            lerp(
                1.0f + max(0.0f, TemporalClampRelaxation),
                1.0f,
                motionConfW);

            float upper =
            min(MomentLuminanceMax, stats.mean + sigmaK * sigma);

            float histLum = SafeLuminance(prevClamped);

            if (histLum > upper)
            {
                float targetLum =
                lerp(histLum, upper, saturate(HistoryClampStrength));

                historyClampAmount =
                saturate(1.0f - targetLum / max(histLum, RT_LUMINANCE_EPS));

                prevClamped = ScaleToLuminance(prevClamped, targetLum);
            }
        }
    }
    
    float prevVarianceForConfidence = 0.0f;

    if (validReuse)
    {
        prevVarianceForConfidence =
        max(prevMoments.y - prevMoments.x * prevMoments.x, 0.0f);
    }

    if (EnableSignalConfidence != 0u && validReuse)
    {
        float currLumForConfidence = SafeLuminance(currColor);
        float prevLumForConfidence = SafeLuminance(prevClamped);

        SignalConfidenceResult conf =
            ComputeSignalConfidence(
              currLumForConfidence,
              prevLumForConfidence,
              prevVarianceForConfidence,
              bestCandidateBaseScore,
              motionConfW,
              bestSurfaceIdMatch,
              bestCandidateViewZWeight,
              historyClampAmount,
              momentClampMask);

        signalConfidence = conf.confidence;
        signalColorConfidence = conf.colorConfidence;
    }
    else if (!validReuse)
    {
        signalConfidence = 0.0f;
        signalColorConfidence = 0.0f;
    }
    
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

    if (EnableSignalConfidence != 0u)
    {
        if (validReuse)
        {
            float confidenceLenScale =
                pow(saturate(signalConfidence), max(1e-4f, HistoryLengthConfidencePower));

            float shapedLen =
                min(MaxStableHistory, (prevLen + 1.0f) * confidenceLenScale);

            newLen = max(1.0f, shapedLen);
        }
        else
        {
            newLen = 1.0f;
        }

        shapedHistoryLenDebug = newLen;
    }

    float alphaUsed = 0.0f;
    float alphaAfterMotionConf = 0.0f;
    float3 history = currColor;
    
    float currLumRaw = SafeLuminance(currColor);

    float currLum =
    (EnableRobustMoments != 0u)
    ? min(currLumRaw, MomentLuminanceMax)
    : currLumRaw;

    float m1 = currLum;
    float m2 = currLum * currLum;

    if (EnableRobustMoments != 0u)
    {
        if (currLumRaw > MomentLuminanceMax)
            momentClampMask = 1.0f;

        if (m2 > MomentVarianceMax)
        {
            m2 = MomentVarianceMax;
            momentClampMask = 1.0f;
        }
    }

    float2 currMoments = float2(m1, m2);
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
        
        if (EnableSignalConfidence != 0u && validReuse)
        {
            float baseAlpha = saturate(alphaUsed);

            // alphaUsed is history weight because final blend is:
            // history = lerp(currColor, prevClamped, alphaUsed)
            // Low confidence should therefore reduce history weight.
            float responsiveAlpha =
                saturate(baseAlpha / (1.0f + ResponsiveAlphaBoost * (1.0f - signalConfidence)));

            float stableAlpha =
                lerp(responsiveAlpha, baseAlpha, signalConfidence);

            float shapedAlpha =
            lerp(
                stableAlpha,
                responsiveAlpha,
                saturate(AntiLagStrength * (1.0f - signalConfidence)));

            antiLagAlphaMultiplier =
                shapedAlpha / max(baseAlpha, 1e-4f);

            alphaUsed = shapedAlpha;
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
        display = bestCandidateViewZWeight.xxx;
    }
    else if (DebugView == 72)
    {
        float mismatch = (bestCandidateViewZWeight < 0.5f) ? 1.0f : 0.0f;
        display = mismatch.xxx;
    }
    
    else if (DebugView == 76)
    {
        display = bestSurfaceIdMatch.xxx;
    }
    
    else if (DebugView == 77)
    {
        display = bestSurfaceIdInvalid.xxx;
    }
    
    else if (DebugView == 84)
    {
        display = historyClampAmount.xxx;
    }
    
    else if (DebugView == 85)
    {
        display = momentClampMask.xxx;
    }
    else if (DebugView == 87)
    {
        display = saturate(signalConfidence * ConfidenceDebugScale).xxx;
    }
    else if (DebugView == 88)
    {
        display = saturate(1.0f - antiLagAlphaMultiplier).xxx;
    }
    else if (DebugView == 89)
    {
        display = saturate(shapedHistoryLenDebug / max(1.0f, MaxStableHistory)).xxx;
    }
    else if (DebugView == 90)
    {
        display = saturate(signalColorConfidence).xxx;
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
        DebugView == 72 ||
        DebugView == 76 ||
        DebugView == 77 ||
        DebugView == 84 ||
        DebugView == 85 ||
        DebugView == 87 ||
        DebugView == 88 ||
        DebugView == 89 ||
        DebugView == 90;

    if (isTemporalDebug)
    {
        g_Output[pixel] = float4(display, 1.0f);
    }
    else
    {
        g_Output[pixel] = float4(LinearToSRGB(history), 1.0f);
    }
}
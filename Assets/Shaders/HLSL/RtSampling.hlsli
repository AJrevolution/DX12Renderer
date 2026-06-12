#ifndef RT_SAMPLING_HLSLI
#define RT_SAMPLING_HLSLI

#include "Common.hlsli"

struct RtEnvAliasEntry
{
    float q;
    uint alias;
    float pdf;
    float weight;
};

float PowerHeuristic(float pdfA, float pdfB)
{
    float a = pdfA * pdfA;
    float b = pdfB * pdfB;
    return a / max(1e-20f, a + b);
}

float PowerHeuristicN(float pdfA, float pdfB, float power)
{
    float p = max(1.0f, power);

    float a = pow(max(pdfA, 0.0f), p);
    float b = pow(max(pdfB, 0.0f), p);

    return a / max(1e-20f, a + b);
}

float CubeTexelSolidAngle(uint2 xy, uint faceSize)
{
    float du = 2.0f / float(faceSize);
    float dv = 2.0f / float(faceSize);

    float u = ((float(xy.x) + 0.5f) / float(faceSize)) * 2.0f - 1.0f;
    float v = ((float(xy.y) + 0.5f) / float(faceSize)) * 2.0f - 1.0f;

    return du * dv / pow(1.0f + u * u + v * v, 1.5f);
}

float3 CubeFaceUVToDirection(uint face, float2 uv)
{
    float u = uv.x * 2.0f - 1.0f;
    float v = uv.y * 2.0f - 1.0f;

    float3 d;

    if (face == 0u)
        d = float3(1.0f, -v, -u);
    else if (face == 1u)
        d = float3(-1.0f, -v, u);
    else if (face == 2u)
        d = float3(u, 1.0f, v);
    else if (face == 3u)
        d = float3(u, -1.0f, -v);
    else if (face == 4u)
        d = float3(u, -v, 1.0f);
    else
        d = float3(-u, -v, -1.0f);

    return normalize(d);
}

uint DirectionToCubeFaceUV(float3 dir, out float2 uv)
{
    float3 a = abs(dir);
    uint face;
    float2 p;

    if (a.x >= a.y && a.x >= a.z)
    {
        if (dir.x > 0.0f)
        {
            face = 0u;
            p = float2(-dir.z, -dir.y) / a.x;
        }
        else
        {
            face = 1u;
            p = float2(dir.z, -dir.y) / a.x;
        }
    }
    else if (a.y >= a.z)
    {
        if (dir.y > 0.0f)
        {
            face = 2u;
            p = float2(dir.x, dir.z) / a.y;
        }
        else
        {
            face = 3u;
            p = float2(dir.x, -dir.z) / a.y;
        }
    }
    else
    {
        if (dir.z > 0.0f)
        {
            face = 4u;
            p = float2(dir.x, -dir.y) / a.z;
        }
        else
        {
            face = 5u;
            p = float2(-dir.x, -dir.y) / a.z;
        }
    }

    uv = saturate(p * 0.5f + 0.5f);
    return face;
}

struct EnvSample
{
    float3 wi;
    float3 Li;
    float pdf;
    uint index;
    bool valid;
};

#endif

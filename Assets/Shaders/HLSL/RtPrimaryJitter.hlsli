#ifndef RT_PRIMARY_JITTER_HLSLI
#define RT_PRIMARY_JITTER_HLSLI

uint RtPrimaryJitterHash(
    uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float RtPrimaryJitterNext01(
    inout uint state)
{
    state =
        1664525u * state +
        1013904223u;

    return
        (state & 0x00FFFFFFu) /
        16777216.0f;
}

float2 RtPrimaryPixelJitter(
    uint2 pixel,
    uint sampleIndex,
    uint resetId)
{
    uint seed =
        pixel.x * 1973u ^
        pixel.y * 9277u ^
        sampleIndex * 26699u ^
        resetId * 31847u ^
        0x68bc21ebu;

    seed =
        RtPrimaryJitterHash(seed);

    return
        float2(
            RtPrimaryJitterNext01(seed),
            RtPrimaryJitterNext01(seed)) -
        0.5f.xx;
}
#endif

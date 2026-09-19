#ifndef RT_RESTIR_REJECTION_HLSLI
#define RT_RESTIR_REJECTION_HLSLI

static const uint RT_RESTIR_REJECT_INVALID_HISTORY = 1u << 0;
static const uint RT_RESTIR_REJECT_INVALID_UV = 1u << 1;
static const uint RT_RESTIR_REJECT_SURFACE_ID = 1u << 2;
static const uint RT_RESTIR_REJECT_NORMAL = 1u << 3;
static const uint RT_RESTIR_REJECT_DEPTH = 1u << 4;
static const uint RT_RESTIR_REJECT_ROUGHNESS = 1u << 5;
static const uint RT_RESTIR_REJECT_VIEWZ = 1u << 6;
static const uint RT_RESTIR_REJECT_AGE = 1u << 7;
static const uint RT_RESTIR_REJECT_RECEIVER = 1u << 8;
static const uint RT_RESTIR_REJECT_TARGET = 1u << 9;
static const uint RT_RESTIR_REJECT_CONFIDENCE = 1u << 10;

float3 RestirRejectionDebugColor(uint reason)
{
    const uint historyReasons =
        RT_RESTIR_REJECT_INVALID_HISTORY |
        RT_RESTIR_REJECT_INVALID_UV |
        RT_RESTIR_REJECT_AGE;

    const uint guideReasons =
        RT_RESTIR_REJECT_SURFACE_ID |
        RT_RESTIR_REJECT_NORMAL |
        RT_RESTIR_REJECT_DEPTH |
        RT_RESTIR_REJECT_ROUGHNESS |
        RT_RESTIR_REJECT_VIEWZ;

    const uint reuseReasons =
        RT_RESTIR_REJECT_RECEIVER |
        RT_RESTIR_REJECT_TARGET |
        RT_RESTIR_REJECT_CONFIDENCE;

    return float3(
        (reason & historyReasons) != 0u ? 1.0f : 0.0f,
        (reason & guideReasons) != 0u ? 1.0f : 0.0f,
        (reason & reuseReasons) != 0u ? 1.0f : 0.0f);
}
#endif

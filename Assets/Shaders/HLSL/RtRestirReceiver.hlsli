#ifndef RT_RESTIR_RECEIVER_HLSLI
#define RT_RESTIR_RECEIVER_HLSLI

static const float
    RT_RESTIR_ROUGHNESS_AOV_EPSILON =
        3.2e-5f;

bool RestirSpecularEligibleFromAov(
    float materialRoughness,
    float cutoff)
{
    return
        materialRoughness >=
        cutoff -
        RT_RESTIR_ROUGHNESS_AOV_EPSILON;
}
#endif

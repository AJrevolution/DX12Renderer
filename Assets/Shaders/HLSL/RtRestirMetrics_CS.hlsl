#include "Common.hlsli"
#include "RtReservoir.hlsli"

Texture2D<float4> g_MeasuredDiffuse : register(t0);
Texture2D<float4> g_MeasuredSpecular : register(t1);

StructuredBuffer<RtRestirEnvReservoirPacked>
    g_FinalReservoir : register(t2);

Texture2D<uint> g_RejectionReason : register(t3);
Texture2D<float4> g_DiffuseAlbedo : register(t4);

cbuffer RtRestirMetricsConstants : register(b0)
{
    uint MeasureReservoirDiagnostics;
    uint DiffuseIsDemodulated;
    uint2 _pad0;
};

struct RtRestirMetricPartial
{
    float diffuseLuminanceSum;
    float specularLuminanceSum;
    float combinedLuminanceSum;
    float maximumLuminance;

    // bits  0..15 = invalid reservoir count
    // bits 16..31 = non-finite signal count
    uint invalidCounts;

    // bits  0..15 = M clamp count
    // bits 16..31 = W clamp count
    uint clampCounts;

    uint rejectionCount;
    uint sampleCount;
};

RWStructuredBuffer<RtRestirMetricPartial>
    g_OutPartials : register(u0);

static const uint RT_RESTIR_METRICS_GROUP_SIZE_X = 16u;
static const uint RT_RESTIR_METRICS_GROUP_SIZE_Y = 16u;
static const uint RT_RESTIR_METRICS_GROUP_THREAD_COUNT = 256u;

groupshared float
    g_GroupDiffuseLuminance[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared float
    g_GroupSpecularLuminance[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared float
    g_GroupCombinedLuminance[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared float
    g_GroupMaximumLuminance[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupReservoirInvalidCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupSignalInvalidCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupMClampCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupWClampCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupRejectionCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

groupshared uint
    g_GroupSampleCount[
        RT_RESTIR_METRICS_GROUP_THREAD_COUNT];

float RtRestirMetricsLuminance(
    float3 value)
{
    value =
        max(
            value,
            0.0f.xxx);

    return
        dot(
            value,
            float3(
                0.2126f,
                0.7152f,
                0.0722f));
}

[numthreads(16, 16, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex)
{
    uint width;
    uint height;

    g_MeasuredDiffuse.GetDimensions(
        width,
        height);

    float diffuseLuminance = 0.0f;
    float specularLuminance = 0.0f;
    float combinedLuminance = 0.0f;
    float maximumLuminance = 0.0f;

    uint reservoirInvalidCount = 0u;
    uint signalInvalidCount = 0u;
    uint mClampCount = 0u;
    uint wClampCount = 0u;
    uint rejectionCount = 0u;
    uint sampleCount = 0u;

    const uint2 pixel =
        dispatchThreadId.xy;

    if (pixel.x < width &&
        pixel.y < height)
    {
        const uint pixelIndex =
            pixel.y * width +
            pixel.x;

        float3 rawDiffuse =
            g_MeasuredDiffuse[pixel].rgb;

        const float3 rawSpecular =
            g_MeasuredSpecular[pixel].rgb;

        // FinalDenoised stores diffuse in demodulated lighting space.
        // Reconstruct the same linear radiance that RtCombine_CS.hlsl
        // sends into the final beauty sum.
        if (DiffuseIsDemodulated != 0u)
        {
            const float4 albedoSample =
                g_DiffuseAlbedo[pixel];

            if (albedoSample.a > 0.5f)
            {
                rawDiffuse *=
                    saturate(
                        albedoSample.rgb);
            }
        }

        const bool diffuseFinite =
            RtReservoirFiniteFloat3(
                rawDiffuse);

        const bool specularFinite =
            RtReservoirFiniteFloat3(
                rawSpecular);

        signalInvalidCount =
            (!diffuseFinite || !specularFinite)
            ? 1u
            : 0u;

        const float3 diffuse =
            diffuseFinite
            ? max(rawDiffuse, 0.0f.xxx)
            : 0.0f.xxx;

        const float3 specular =
            specularFinite
            ? max(rawSpecular, 0.0f.xxx)
            : 0.0f.xxx;

        diffuseLuminance =
            RtRestirMetricsLuminance(
                diffuse);

        specularLuminance =
            RtRestirMetricsLuminance(
                specular);

        combinedLuminance =
            RtRestirMetricsLuminance(
                diffuse +
                specular);

        maximumLuminance =
            combinedLuminance;

        if (MeasureReservoirDiagnostics != 0u)
        {
            const RtRestirEnvReservoirPacked reservoir =
                g_FinalReservoir[pixelIndex];

            if (!ReservoirFinalizedValid(
                reservoir))
            {
                reservoirInvalidCount = 1u;
            }

            const uint reservoirFlags =
                ReservoirFlags(
                    reservoir);

            if ((reservoirFlags &
                RT_RESTIR_RESERVOIR_M_CLAMPED) != 0u)
            {
                mClampCount = 1u;
            }

            if ((reservoirFlags &
                RT_RESTIR_RESERVOIR_W_CLAMPED) != 0u)
            {
                wClampCount = 1u;
            }

            if (g_RejectionReason[pixel] != 0u)
            {
                rejectionCount = 1u;
            }
        }

        sampleCount = 1u;
    }

    g_GroupDiffuseLuminance[groupIndex] =
        diffuseLuminance;

    g_GroupSpecularLuminance[groupIndex] =
        specularLuminance;

    g_GroupCombinedLuminance[groupIndex] =
        combinedLuminance;

    g_GroupMaximumLuminance[groupIndex] =
        maximumLuminance;

    g_GroupReservoirInvalidCount[groupIndex] =
        reservoirInvalidCount;

    g_GroupSignalInvalidCount[groupIndex] =
        signalInvalidCount;

    g_GroupMClampCount[groupIndex] =
        mClampCount;

    g_GroupWClampCount[groupIndex] =
        wClampCount;

    g_GroupRejectionCount[groupIndex] =
        rejectionCount;

    g_GroupSampleCount[groupIndex] =
        sampleCount;

    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 128u;
        stride > 0u;
        stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            const uint otherIndex =
                groupIndex +
                stride;

            g_GroupDiffuseLuminance[groupIndex] +=
                g_GroupDiffuseLuminance[otherIndex];

            g_GroupSpecularLuminance[groupIndex] +=
                g_GroupSpecularLuminance[otherIndex];

            g_GroupCombinedLuminance[groupIndex] +=
                g_GroupCombinedLuminance[otherIndex];

            g_GroupMaximumLuminance[groupIndex] =
                max(
                    g_GroupMaximumLuminance[groupIndex],
                    g_GroupMaximumLuminance[otherIndex]);

            g_GroupReservoirInvalidCount[groupIndex] +=
                g_GroupReservoirInvalidCount[otherIndex];

            g_GroupSignalInvalidCount[groupIndex] +=
                g_GroupSignalInvalidCount[otherIndex];

            g_GroupMClampCount[groupIndex] +=
                g_GroupMClampCount[otherIndex];

            g_GroupWClampCount[groupIndex] +=
                g_GroupWClampCount[otherIndex];

            g_GroupRejectionCount[groupIndex] +=
                g_GroupRejectionCount[otherIndex];

            g_GroupSampleCount[groupIndex] +=
                g_GroupSampleCount[otherIndex];
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0u)
    {
        const uint groupCountX =
            (width +
                RT_RESTIR_METRICS_GROUP_SIZE_X -
                1u) /
            RT_RESTIR_METRICS_GROUP_SIZE_X;

        const uint partialIndex =
            groupId.y *
                groupCountX +
            groupId.x;

        RtRestirMetricPartial partial;

        partial.diffuseLuminanceSum =
            g_GroupDiffuseLuminance[0];

        partial.specularLuminanceSum =
            g_GroupSpecularLuminance[0];

        partial.combinedLuminanceSum =
            g_GroupCombinedLuminance[0];

        partial.maximumLuminance =
            g_GroupMaximumLuminance[0];

        // A 16x16 group contains at most 256 pixels,
        // so both counters safely fit in 16 bits.
        partial.invalidCounts =
            (g_GroupReservoirInvalidCount[0] & 0xFFFFu) |
            ((g_GroupSignalInvalidCount[0] & 0xFFFFu) << 16u);

        // A 16x16 group contains at most 256 pixels, so each count fits
        // comfortably in 16 bits.
        partial.clampCounts =
            (g_GroupMClampCount[0] & 0xFFFFu) |
            ((g_GroupWClampCount[0] & 0xFFFFu) << 16u);

        partial.rejectionCount =
            g_GroupRejectionCount[0];

        partial.sampleCount =
            g_GroupSampleCount[0];

        g_OutPartials[partialIndex] =
            partial;
    }
}

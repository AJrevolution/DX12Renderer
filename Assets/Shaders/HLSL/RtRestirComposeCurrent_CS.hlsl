#include "RtRestirReceiver.hlsli"

Texture2D<float4> g_BaseDiffuse : register(t0);
Texture2D<float4> g_BaseSpecular : register(t1);

Texture2D<float4> g_RestirDiffuse : register(t2);
Texture2D<float4> g_RestirSpecular : register(t3);

Texture2D<float4> g_RestirReceiverNormalRoughness : register(t4);

Texture2D<float4> g_ExactTargetReferenceDiffuse : register(t5);
Texture2D<float4> g_ExactTargetReferenceSpec : register(t6);

RWTexture2D<float4> g_ComposedDiffuse : register(u0);
RWTexture2D<float4> g_ComposedSpecular : register(u1);
RWTexture2D<float4> g_DebugOutput : register(u2);

static const uint RT_DIRECT_ENV_LEGACY_MIS = 0u;
static const uint RT_DIRECT_ENV_RESTIR_VALIDATION = 1u;
static const uint RT_DIRECT_ENV_RESTIR_PRODUCTION = 2u;

cbuffer RtRestirComposeCurrentConstants : register(b0)
{
    uint DirectEnvironmentMode;
    uint DebugView;

    float DeltaRoughnessCutoff;
    uint HasExactTargetReference;
};

bool Finite3(float3 value)
{
    return
        !any(isnan(value)) &&
        !any(isinf(value));
}

float3 SanitizeRadiance(float3 value)
{
    return
        Finite3(value)
        ? max(value, 0.0f.xxx)
        : 0.0f.xxx;
}

float SafeLuminance(float3 value)
{
    return dot(
        SanitizeRadiance(value),
        float3(
            0.2126f,
            0.7152f,
            0.0722f));
}

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId :
        SV_DispatchThreadID)
{
    const uint2 pixel =
        dispatchThreadId.xy;

    uint width;
    uint height;

    g_ComposedDiffuse.GetDimensions(
        width,
        height);

    if (pixel.x >= width ||
        pixel.y >= height)
    {
        return;
    }

    const float4 baseDiffuse =
        g_BaseDiffuse[pixel];

    const float4 baseSpecular =
        g_BaseSpecular[pixel];

    const float3 restirDiffuse =
        SanitizeRadiance(
            g_RestirDiffuse[pixel].rgb);

    float3 restirSpecular =
        SanitizeRadiance(
            g_RestirSpecular[pixel].rgb);

    const float materialRoughness =
        saturate(
            g_RestirReceiverNormalRoughness[pixel].a);
    
    const bool specularEligible =
        RestirSpecularEligibleFromAov(
            materialRoughness,
            DeltaRoughnessCutoff);

    // Defensive ownership enforcement. Resolve should already output zero
    // below this boundary.
    if (!specularEligible)
    {
        restirSpecular = 0.0f.xxx;
    }

    const float3 composedDiffuse =
        SanitizeRadiance(
            baseDiffuse.rgb) +
        restirDiffuse;

    const float3 composedSpecular =
        SanitizeRadiance(
            baseSpecular.rgb) +
        restirSpecular;

    g_ComposedDiffuse[pixel] =
        float4(
            composedDiffuse,
            baseDiffuse.a);

    g_ComposedSpecular[pixel] =
        float4(
            composedSpecular,
            baseSpecular.a);

    if (DebugView == 120u)
    {
        const bool restirOwnsEnvironment =
            DirectEnvironmentMode ==
                RT_DIRECT_ENV_RESTIR_PRODUCTION;

        const float diffuseOwned =
            restirOwnsEnvironment
            ? 1.0f
            : 0.0f;

        const float specularOwned =
            restirOwnsEnvironment &&
            specularEligible
            ? 1.0f
            : 0.0f;

        const float deltaFallback =
            restirOwnsEnvironment &&
            !specularEligible
            ? 1.0f
            : 0.0f;

        // R = ReSTIR diffuse ownership
        // G = ReSTIR non-delta specular ownership
        // B = legacy near-delta specular fallback
        g_DebugOutput[pixel] =
            float4(
                diffuseOwned,
                specularOwned,
                deltaFallback,
                1.0f);
    }
    else if (DebugView == 121u)
    {
        if (HasExactTargetReference == 0u)
        {
            // Magenta = exact-target comparison reference unavailable.
            g_DebugOutput[pixel] =
                float4(
                    1.0f,
                    0.0f,
                    1.0f,
                    1.0f);
        }
        else
        {
            const float3 exactTargetDiffuse =
                SanitizeRadiance(
                    g_ExactTargetReferenceDiffuse[pixel].rgb);

            float3 exactTargetSpec =
                SanitizeRadiance(
                    g_ExactTargetReferenceSpec[pixel].rgb);

            // Compare only the environment domain actually owned by ReSTIR.
            // Near-delta specular intentionally remains on the legacy beauty path.
            if (!specularEligible)
            {
                exactTargetSpec = 0.0f.xxx;
            }

            const float3 exactTarget =
                exactTargetDiffuse +
                exactTargetSpec;

            const float3 restir =
                restirDiffuse +
                restirSpecular;

            const float exactTargetLum =
                SafeLuminance(exactTarget);

            const float restirLum =
                SafeLuminance(restir);

            const float relativeError =
                abs(
                    restirLum -
                    exactTargetLum) /
                max(
                    1.0e-4f,
                    exactTargetLum);

            g_DebugOutput[pixel] =
                float4(
                    saturate(relativeError),
                    saturate(
                        relativeError *
                        0.25f),
                    0.0f,
                    1.0f);
        }
    }
    else if (DebugView == 123u)
    {
        g_DebugOutput[pixel] =
            float4(
                composedDiffuse,
                1.0f);
    }
    else if (DebugView == 124u)
    {
        g_DebugOutput[pixel] =
            float4(
                composedSpecular,
                1.0f);
    }
}

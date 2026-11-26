#include "CBufferStructures.hlsli"
#include "PBRMath.hlsli"
#include "ShadowSampling.hlsli"

ConstantBuffer<LightGridCB> Grid : register(b0);
cbuffer OptionalInputs : register(b1)
{
    uint HasDirectionalShadows;
    uint HasPointSpotShadows;
    uint HasAmbientOcclusion;
};
ConstantBuffer<CameraCB> Camera : register(b2);
Texture2D<float4> AlbedoMetallic : register(t0);
Texture2D<float4> NormalRoughness : register(t1);
Texture2D<float4> F0Baked : register(t2);
Texture2D<float4> Emissive : register(t3);
Texture2D<float> Depth : register(t4);
TextureCube<float4> IrradianceEnvironment : register(t5);
TextureCube<float4> PrefilteredEnvironment : register(t6);
Texture2D<float4> EnvironmentBRDF : register(t7);
StructuredBuffer<GDX12LightConstants> LightsCache : register(t8);
StructuredBuffer<uint> ClusterLightMasks : register(t9);
Texture2DArray<float> DirectionalShadowMaps : register(t10);
Texture2DArray<float> PointSpotShadowMaps : register(t11);
StructuredBuffer<ShadowView> ShadowViews : register(t12);
Texture2D<float> AmbientOcclusion : register(t13);
RWTexture2D<unorm float4> SceneColor : register(u0);
SamplerState samLinearClamp : register(s3);
SamplerComparisonState samShadow : register(s6);

float3 EvaluateBRDF(PBRSurface s, float3 v, float3 l)
{
    float nDotV = saturate(dot(s.Normal, v)), nDotL = saturate(dot(s.Normal, l));
    float3 result = 0;
    if (nDotV > 0 && nDotL > 0)
    {
        float3 h = SafeNormalize(v + l);
        float3 fresnel = s.F0 + (1 - s.F0) * FresnelSchlickFactor(saturate(dot(v, h)));
        float3 diffuse = (1 - fresnel) * (1 - s.Metallic) * s.Albedo / PBR_PI;
        float3 specular = DistributionGGX(saturate(dot(s.Normal, h)), s.Roughness) * SmithGGXVisibility(nDotV, nDotL, s.Roughness) * fresnel;
        result = (diffuse + specular) * nDotL;
    }
    return result;
}

float3 EnvironmentLighting(PBRSurface s, float3 v, float ambientVisibility)
{
    float nDotV = saturate(dot(s.Normal, v));
    float3 irradiance = IrradianceEnvironment.SampleLevel(samLinearClamp, s.Normal, 0).rgb;
    float3 prefiltered = PrefilteredEnvironment.SampleLevel(samLinearClamp, reflect(-v, s.Normal), s.Roughness * 7).rgb;

    float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, s.Roughness), 0).rg;
    float3 fresnel = s.F0 + (max(1 - s.Roughness, s.F0) - s.F0) * FresnelSchlickFactor(nDotV);
    float3 diffuseIBL = (1 - fresnel) * (1 - s.Metallic) * irradiance * s.Albedo / PBR_PI;
    float3 specularIBL = prefiltered * (s.F0 * brdf.x + brdf.y);
    // Filament's Lagarde/de Rousiers specular occlusion.
    float linearRoughness = s.Roughness * s.Roughness;
    float specularVisibility = saturate(pow(nDotV + ambientVisibility, exp2(-16.0f * linearRoughness - 1.0f)) - 1.0f + ambientVisibility);
    return diffuseIBL * ambientVisibility + specularIBL * specularVisibility;
}

float3 ShadeLight(PBRSurface s, float3 position, float3 v, GDX12LightConstants light, float viewDepth)
{
    float3 radiance = light.Color * light.Intensity;
    float3 result = 0;
    float3 l = -light.Direction;
    if (light.Type == LIGHT_TYPE_DIRECTIONAL)
    {
        result = EvaluateBRDF(s, v, l) * radiance;
    }
    else
    {
        float3 delta = light.Position - position;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared < light.Range * light.Range)
        {
            l = delta * rsqrt(max(distanceSquared, 1e-6f));
            float cone = light.Type == LIGHT_TYPE_SPOT ? saturate((dot(-l, light.Direction) - light.OuterConeCos) / max(light.InnerConeCos - light.OuterConeCos, 1e-4f)) : 1;
            float rangeFade = saturate(1 - pow(distanceSquared / (light.Range * light.Range), 2));
            // Units are centimetres, intensity is expressed per square metre.
            float attenuation = rangeFade * rangeFade * cone * cone / max(distanceSquared * 0.0001f, 0.01f);
            result = EvaluateBRDF(s, v, l) * radiance * attenuation;
        }
    }
    bool hasShadows = light.Type == LIGHT_TYPE_DIRECTIONAL ? HasDirectionalShadows != 0 : HasPointSpotShadows != 0;
    if (hasShadows && any(result > 0))
        result *= ShadowVisibility(DirectionalShadowMaps, PointSpotShadowMaps, ShadowViews, samShadow,
            light, position, s.Normal, saturate(dot(s.Normal, l)), viewDepth, Camera.NearPlane);
    return result;
}

float3 ShadeSurface(PBRSurface s, float3 emissive, float3 position, uint2 pixel, float3 cameraPosition, float viewDepth)
{
    float3 v = SafeNormalize(cameraPosition - position);
    float ambientVisibility = HasAmbientOcclusion ? AmbientOcclusion[pixel] : 1.0f;
    float3 result = EnvironmentLighting(s, v, ambientVisibility) + emissive;
    if (Grid.MaskWordsPerCluster > 0)
    {
        uint slice = (uint)clamp(log2(viewDepth) * Grid.DepthSliceScale + Grid.DepthSliceBias, 0, (float)(Grid.DepthSlices - 1));
        uint2 tile = min(pixel / Grid.TileSize, uint2(Grid.TileColumns, Grid.TileRows) - 1);
        uint base = ((slice * Grid.TileRows + tile.y) * Grid.TileColumns + tile.x) * Grid.MaskWordsPerCluster;
        for (uint word = 0; word < Grid.MaskWordsPerCluster; ++word)
        {
            uint mask = ClusterLightMasks[base + word];
            while (mask)
            {
                uint bit = firstbitlow(mask);
                mask &= mask - 1;
                result += ShadeLight(s, position, v, LightsCache[word * 32 + bit], viewDepth);
            }
        }
    }
    return result;
}

[numthreads(8, 8, 1)]
void CS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    SceneColor.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float depth = Depth[pixel];
    if (depth <= 0) { SceneColor[pixel] = float4(0, 0, 0, 1); return; }
    float4 albedo = AlbedoMetallic[pixel], f0 = F0Baked[pixel];
    if (f0.a > 0.5f) { SceneColor[pixel] = float4(ToneMapToSRGB(albedo.rgb), 1); return; }

    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    float3 position = world.xyz / world.w;
    float4 normal = NormalRoughness[pixel];
    PBRSurface s;
    s.Albedo = albedo.rgb;
    s.Metallic = albedo.a;
    s.F0 = f0.rgb;
    s.Normal = SafeNormalize(normal.xyz);
    s.Roughness = normal.w;
    float viewDepth = max(-mul(float4(position, 1), Camera.View).z, Camera.NearPlane);
    float3 result = ShadeSurface(s, Emissive[pixel].rgb, position, pixel, Camera.CameraLocation, viewDepth);
    SceneColor[pixel] = float4(ToneMapToSRGB(result), 1);
}

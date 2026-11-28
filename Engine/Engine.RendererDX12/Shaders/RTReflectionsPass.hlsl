#include "CBufferStructures.hlsli"
#include "RayTracing.hlsli"
#include "PBRMath.hlsli"
#include "ShadowSampling.hlsli"

cbuffer Parameters : register(b0)
{
    float MaxDistance, MaxRoughness, RoughnessFade, Bias, Intensity;
    uint SamplesPerPixel, FrameIndex, HasDirectionalShadows, HasPointSpotShadows;
    uint HasAmbientOcclusion, HasPrefiltered, HasBRDF, LightCount;
};
ConstantBuffer<CameraCB> Camera : register(b1);

Texture2D<float4> InputColor : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float4> F0Baked : register(t3);
RaytracingAccelerationStructure Scene : register(t4);
StructuredBuffer<GeometryBufferVertex> Vertices : register(t5);
Buffer<uint> Indices : register(t6);
StructuredBuffer<IndirectDrawArgs> Draws : register(t7);
StructuredBuffer<InstanceData> Instances : register(t8);
StructuredBuffer<Transform> Transforms : register(t9);
StructuredBuffer<Material> Materials : register(t10);
StructuredBuffer<GDX12LightConstants> Lights : register(t11);
TextureCube<float4> SkyBoxMap : register(t12);
TextureCube<float4> IrradianceEnvironment : register(t13);
TextureCube<float4> PrefilteredEnvironment : register(t14);
Texture2D<float4> EnvironmentBRDF : register(t15);
Texture2DArray<float> DirectionalShadowMaps : register(t16);
Texture2DArray<float> PointSpotShadowMaps : register(t17);
StructuredBuffer<ShadowView> ShadowViews : register(t18);
Texture2D<float> AmbientOcclusion : register(t19);
Texture2D<float4> ReflectionInput : register(t20);
Texture2D<float4> Texture2DCache[TEXTURE2D_RANGE_LENGTH] : register(t21);
RWTexture2D<float4> ReflectionOutput : register(u0);
RWTexture2D<float4> SceneColor : register(u1);
SamplerState samLinearWrap : register(s2);
SamplerState samLinearClamp : register(s3);
SamplerComparisonState samShadow : register(s6);

float3 ReconstructPosition(uint2 pixel, uint2 size, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / size * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    return world.xyz / world.w;
}

float ReflectionWeight(uint2 pixel)
{
    float4 normal = NormalRoughness[pixel], f0 = F0Baked[pixel];
    if (Intensity <= 0 || Depth[pixel] <= 0 || f0.a > 0.5f || normal.w >= MaxRoughness
        || dot(normal.xyz, normal.xyz) < 1e-8f || !any(f0.rgb > 0)) return 0;
    return RoughnessFade > 0
        ? 1 - smoothstep(max(0, MaxRoughness - RoughnessFade), MaxRoughness, normal.w) : 1;
}

#if __SHADER_TARGET_MINOR >= 7
struct [raypayload] ReflectionPayload
{
    float3 Radiance : read(caller) : write(caller, closesthit, miss);
    float ConeWidth : read(closesthit, anyhit) : write(caller);
    float ConeSpread : read(closesthit, anyhit, miss) : write(caller);
};
#else
struct ReflectionPayload { float3 Radiance; float ConeWidth; float ConeSpread; };
#endif

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float2 SampleSequence(uint2 pixel, uint sampleIndex)
{
    uint seed = Hash(pixel.x ^ Hash(pixel.y));
    float2 phase = float2(seed & 0xffffu, seed >> 16) / 65536.0f;
    float index = FrameIndex * SamplesPerPixel + sampleIndex;
    return frac(phase + index * float2(0.754877666f, 0.569840291f));
}

float3 SampleReflectionDirection(float3 normal, float3 view, float roughness, float2 xi, out float3 halfVector)
{
    float cosTheta = GGXSampleCosTheta(xi.y, roughness);
    float sinTheta = sqrt(saturate(1 - cosTheta * cosTheta));
    float phi = 2 * PBR_PI * xi.x;
    float3 up = abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = SafeNormalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    halfVector = SafeNormalize(tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + normal * cosTheta);
    return reflect(-view, halfVector);
}

// Triangle edges provide the UV footprint and normal-map frame without raster derivatives.
void HitTextureFrame(RayTracingHitSurface surface, uint instanceID, uint geometryIndex, uint primitiveIndex,
    float footprint, out float3 tangent, out float3 bitangent, out float2 uvFootprint)
{
    uint instanceIndex = instanceID + geometryIndex;
    IndirectDrawArgs draw = Draws[instanceIndex];
    uint first = draw.StartIndexLocation + primitiveIndex * 3;
    GeometryBufferVertex v0 = Vertices[int(Indices[first]) + draw.BaseVertexLocation];
    GeometryBufferVertex v1 = Vertices[int(Indices[first + 1]) + draw.BaseVertexLocation];
    GeometryBufferVertex v2 = Vertices[int(Indices[first + 2]) + draw.BaseVertexLocation];
    float3x3 world = (float3x3)Transforms[Instances[instanceIndex].TransformIndex].World;
    float3 edge1 = mul(v1.Position - v0.Position, world), edge2 = mul(v2.Position - v0.Position, world);
    float2 uv1 = v1.TexCoord - v0.TexCoord, uv2 = v2.TexCoord - v0.TexCoord;
    float3 geometricNormal = SafeNormalize(cross(edge1, edge2));
    tangent = cross(edge2, surface.WorldNormal) * uv1.x + cross(surface.WorldNormal, edge1) * uv2.x;
    bitangent = cross(edge2, surface.WorldNormal) * uv1.y + cross(surface.WorldNormal, edge1) * uv2.y;
    float area = max(length(cross(edge1, edge2)), 1e-8f);
    float incidence = max(abs(dot(geometricNormal, WorldRayDirection())), 0.05f);
    uvFootprint = footprint / (area * incidence) * float2(length(tangent), length(bitangent));
}

float4 SampleHitTexture(uint textureIndex, float2 uv, float2 uvFootprint)
{
    uint width, height, levels;
    uint index = NonUniformResourceIndex(textureIndex);
    Texture2DCache[index].GetDimensions(0, width, height, levels);
    float lod = log2(max(max(uvFootprint.x * width, uvFootprint.y * height), 1.0f));
    return Texture2DCache[index].SampleLevel(samLinearWrap, uv, min(lod, float(levels - 1)));
}

float3 EvaluateBRDF(PBRSurface surface, float3 view, float3 light)
{
    float nDotV = saturate(dot(surface.Normal, view)), nDotL = saturate(dot(surface.Normal, light));
    if (nDotV <= 0 || nDotL <= 0) return 0;
    float3 halfVector = SafeNormalize(view + light);
    float3 fresnel = surface.F0 + (1 - surface.F0) * FresnelSchlickFactor(saturate(dot(view, halfVector)));
    float3 diffuse = (1 - fresnel) * (1 - surface.Metallic) * surface.Albedo / PBR_PI;
    float3 specular = DistributionGGX(saturate(dot(surface.Normal, halfVector)), surface.Roughness)
        * SmithGGXVisibility(nDotV, nDotL, surface.Roughness) * fresnel;
    return (diffuse + specular) * nDotL;
}

float3 EnvironmentLighting(PBRSurface surface, float3 view)
{
    float nDotV = saturate(dot(surface.Normal, view));
    float3 irradiance = IrradianceEnvironment.SampleLevel(samLinearClamp, surface.Normal, 0).rgb;
    float3 prefiltered = PrefilteredEnvironment.SampleLevel(samLinearClamp,
        reflect(-view, surface.Normal), surface.Roughness * 7).rgb;
    float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, surface.Roughness), 0).rg;
    float3 fresnel = surface.F0 + (max(1 - surface.Roughness, surface.F0) - surface.F0) * FresnelSchlickFactor(nDotV);
    float3 diffuse = (1 - fresnel) * (1 - surface.Metallic) * irradiance * surface.Albedo / PBR_PI;
    return diffuse + prefiltered * (surface.F0 * brdf.x + brdf.y);
}

float3 ShadeLight(PBRSurface surface, float3 position, float3 view, GDX12LightConstants light)
{
    if (light.Intensity <= 0) return 0;
    float3 direction = -light.Direction;
    float attenuation = 1;
    if (light.Type != LIGHT_TYPE_DIRECTIONAL)
    {
        float3 delta = light.Position - position;
        float distanceSquared = dot(delta, delta);
        if (light.Range <= 0 || distanceSquared >= light.Range * light.Range) return 0;
        direction = delta * rsqrt(max(distanceSquared, 1e-6f));
        float cone = light.Type == LIGHT_TYPE_SPOT ? saturate((dot(-direction, light.Direction) - light.OuterConeCos)
            / max(light.InnerConeCos - light.OuterConeCos, 1e-4f)) : 1;
        float rangeFade = saturate(1 - pow(distanceSquared / (light.Range * light.Range), 2));
        attenuation = rangeFade * rangeFade * cone * cone / max(distanceSquared * 0.0001f, 0.01f);
    }
    float3 result = EvaluateBRDF(surface, view, direction) * light.Color * light.Intensity * attenuation;
    bool hasShadows = light.Type == LIGHT_TYPE_DIRECTIONAL ? HasDirectionalShadows != 0 : HasPointSpotShadows != 0;
    if (hasShadows && any(result > 0))
    {
        float viewDepth = -mul(float4(position, 1), Camera.View).z;
        result *= ShadowVisibility(DirectionalShadowMaps, PointSpotShadowMaps, ShadowViews, samShadow,
            light, position, surface.Normal, saturate(dot(surface.Normal, direction)), viewDepth, Camera.NearPlane);
    }
    return result;
}

[shader("anyhit")]
void AnyHit(inout ReflectionPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    Material material = Materials[Instances[InstanceID() + GeometryIndex()].MaterialIndex];
    if (material.RenderLayer == RENDER_LAYER_TRANSPARENT) { IgnoreHit(); return; }
    if (material.RenderLayer != RENDER_LAYER_MASKED || material.DiffuseMapIndex == NO_TEXTURE) return;
    RayTracingHitSurface surface = LoadRayTracingHitSurface(Vertices, Indices, Draws, Instances, Transforms,
        InstanceID(), GeometryIndex(), PrimitiveIndex(), attributes.barycentrics);
    float3 tangent, bitangent;
    float2 uvFootprint;
    HitTextureFrame(surface, InstanceID(), GeometryIndex(), PrimitiveIndex(),
        payload.ConeWidth + RayTCurrent() * payload.ConeSpread, tangent, bitangent, uvFootprint);
    if (SampleHitTexture(material.DiffuseMapIndex, surface.TexCoord, uvFootprint).a < 0.5f) IgnoreHit();
}

[shader("closesthit")]
void ClosestHit(inout ReflectionPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.Radiance = 0;
    RayTracingHitSurface hit = LoadRayTracingHitSurface(Vertices, Indices, Draws, Instances, Transforms,
        InstanceID(), GeometryIndex(), PrimitiveIndex(), attributes.barycentrics);
    Material material = Materials[hit.MaterialIndex];
    float3 tangent, bitangent;
    float2 uvFootprint;
    HitTextureFrame(hit, InstanceID(), GeometryIndex(), PrimitiveIndex(),
        payload.ConeWidth + RayTCurrent() * payload.ConeSpread, tangent, bitangent, uvFootprint);
    float3 albedo = material.DiffuseMapIndex != NO_TEXTURE
        ? SampleHitTexture(material.DiffuseMapIndex, hit.TexCoord, uvFootprint).rgb : 1;
    if (material.UseBakedLighting != 0) { payload.Radiance = albedo; return; }
    PBRSurface surface;
    surface.Albedo = albedo;
    surface.Normal = hit.WorldNormal;
    if (material.NormalMapIndex != NO_TEXTURE)
    {
        float frameLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
        if (frameLength > 1e-12f)
        {
            float3 mapped = SampleHitTexture(material.NormalMapIndex, hit.TexCoord, uvFootprint).xyz * 2 - 1;
            surface.Normal = SafeNormalize((mapped.x * tangent + mapped.y * bitangent) * rsqrt(frameLength) + mapped.z * surface.Normal);
        }
    }
    float3 view = -WorldRayDirection();
    if (dot(surface.Normal, view) < 0) surface.Normal = -surface.Normal;
    surface.Roughness = material.Roughness;
    if (material.RoughnessMapIndex != NO_TEXTURE)
        surface.Roughness *= SampleHitTexture(material.RoughnessMapIndex, hit.TexCoord, uvFootprint).r;
    surface.Roughness = clamp(surface.Roughness, 0.045f, 1.0f);
    surface.Metallic = saturate(material.Metallic);
    float3 specular = saturate(material.SpecularColor);
    if (material.SpecularMapIndex != NO_TEXTURE)
        specular *= SampleHitTexture(material.SpecularMapIndex, hit.TexCoord, uvFootprint).rgb;
    surface.F0 = lerp(specular, albedo, surface.Metallic);
    float3 emissive = material.EmissiveColor;
    if (material.EmissiveMapIndex != NO_TEXTURE)
        emissive *= SampleHitTexture(material.EmissiveMapIndex, hit.TexCoord, uvFootprint).rgb;
    float3 radiance = EnvironmentLighting(surface, view) + emissive;
    for (uint i = 0; i < LightCount; ++i)
        radiance += ShadeLight(surface, hit.WorldPosition, view, Lights[i]);
    payload.Radiance = radiance;
}

[shader("miss")]
void Miss(inout ReflectionPayload payload)
{
    uint width, height, levels;
    SkyBoxMap.GetDimensions(0, width, height, levels);
    float lod = clamp(log2(max(payload.ConeSpread * width * 0.5f, 1.0f)), 0, float(levels - 1));
    payload.Radiance = SkyBoxMap.SampleLevel(samLinearClamp, WorldRayDirection(), lod).rgb;
}

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pixel = DispatchRaysIndex().xy, size = DispatchRaysDimensions().xy;
    float weight = ReflectionWeight(pixel);
    if (weight <= 0) { ReflectionOutput[pixel] = 0; return; }
    float depth = Depth[pixel];
    float3 position = ReconstructPosition(pixel, size, depth);
    float3 normal = SafeNormalize(NormalRoughness[pixel].xyz);
    float roughness = clamp(NormalRoughness[pixel].w, 0.045f, 1.0f);
    float3 view = SafeNormalize(Camera.CameraLocation - position);
    float nDotV = saturate(dot(normal, view));
    if (nDotV <= 0) { ReflectionOutput[pixel] = 0; return; }
    float3 f0 = F0Baked[pixel].rgb;
    float footprint = max(length(ReconstructPosition(pixel + uint2(1, 0), size, depth) - position),
        length(ReconstructPosition(pixel + uint2(0, 1), size, depth) - position));
    float3 contribution = 0;
    for (uint sampleIndex = 0; sampleIndex < SamplesPerPixel; ++sampleIndex)
    {
        float3 halfVector;
        float3 direction = SampleReflectionDirection(normal, view, roughness, SampleSequence(pixel, sampleIndex), halfVector);
        float nDotL = saturate(dot(normal, direction)), vDotH = dot(view, halfVector);
        if (nDotL <= 0 || vDotH <= 0) continue;
        RayDesc ray;
        ray.Origin = position + normal * Bias;
        ray.Direction = direction;
        ray.TMin = 0;
        ray.TMax = MaxDistance;
        ReflectionPayload payload;
        payload.Radiance = 0;
        payload.ConeWidth = footprint;
        payload.ConeSpread = footprint / max(length(Camera.CameraLocation - position), 1e-4f);
        TraceRay(Scene, RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xff, 0, 0, 0, ray, payload);
        float3 fresnel = f0 + (1 - f0) * FresnelSchlickFactor(saturate(vDotH));
        contribution += payload.Radiance * fresnel * SplitSumVisibility(nDotV, nDotL,
            saturate(dot(normal, halfVector)), vDotH, roughness);
    }
    contribution /= SamplesPerPixel;
    ReflectionOutput[pixel] = float4(min(max(contribution, 0), 65504.0f), weight);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComposeCS(uint3 thread : SV_DispatchThreadID)
{
    uint2 size, pixel = thread.xy;
    SceneColor.GetDimensions(size.x, size.y);
    if (any(pixel >= size)) return;
    float4 scene = InputColor[pixel], reflection = ReflectionInput[pixel];
    if (reflection.a > 0)
    {
        float3 position = ReconstructPosition(pixel, size, Depth[pixel]);
        float3 view = SafeNormalize(Camera.CameraLocation - position);
        float3 normal = SafeNormalize(NormalRoughness[pixel].xyz);
        float roughness = NormalRoughness[pixel].w;
        float nDotV = saturate(dot(normal, view));
        float3 fallback = 0;
        if (HasPrefiltered && HasBRDF)
        {
            float3 radiance = PrefilteredEnvironment.SampleLevel(samLinearClamp, reflect(-view, normal), roughness * 7).rgb;
            float2 brdf = EnvironmentBRDF.SampleLevel(samLinearClamp, float2(nDotV, roughness), 0).rg;
            fallback = radiance * (F0Baked[pixel].rgb * brdf.x + brdf.y);
        }
        float ambientVisibility = HasAmbientOcclusion ? AmbientOcclusion[pixel] : 1;
        float specularVisibility = saturate(pow(nDotV + ambientVisibility,
            exp2(-16 * roughness * roughness - 1)) - 1 + ambientVisibility);

        scene.rgb = min(max(0, scene.rgb + Intensity * reflection.a * specularVisibility * (reflection.rgb - fallback)), 65504.0f);
    }
    SceneColor[pixel] = scene;
}

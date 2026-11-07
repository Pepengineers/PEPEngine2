#include "Particles.hlsli"

StructuredBuffer<ParticleDraw> Draws : register(t0);
StructuredBuffer<uint> Indices : register(t1);
StructuredBuffer<uint> SortedIndices : register(t2);
Texture2D<float> SceneDepth : register(t3);
Texture2D<float4> Textures[TEXTURE2D_RANGE_LENGTH] : register(t4);
SamplerState LinearClamp : register(s0);

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Current : TEXCOORD1;
    float4 Previous : TEXCOORD2;
    nointerpolation uint DrawIndex : TEXCOORD3;
    float4 Material : TEXCOORD4;
    float3 Normal : TEXCOORD5;
    float ViewDepth : TEXCOORD6;
};

VertexOutput VS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const float2 corners[6] = { float2(-0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, -0.5f),
        float2(0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, 0.5f) };
    uint index = BurstCount == 2 ? SortedIndices[instance] : Indices[Capacity + instance];
    ParticleDraw draw = Draws[index];
    float2 corner = corners[vertex];
    float2 rotated = float2(corner.x * cos(draw.Rotation) - corner.y * sin(draw.Rotation),
        corner.x * sin(draw.Rotation) + corner.y * cos(draw.Rotation));
    float2 previousRotated = float2(corner.x * cos(draw.PreviousRotation) - corner.y * sin(draw.PreviousRotation),
        corner.x * sin(draw.PreviousRotation) + corner.y * cos(draw.PreviousRotation));
    if (draw.Geometry == ParticleGeometryWaterDrop)
    {
        rotated.y *= 2.5f;
        previousRotated.y *= 2.5f;
    }
    float3 right = float3(Camera.View[0][0], Camera.View[1][0], Camera.View[2][0]);
    float3 up = float3(Camera.View[0][1], Camera.View[1][1], Camera.View[2][1]);
    float3 previousRight = normalize(float3(Camera.PrevViewProjNoJitter[0][0], Camera.PrevViewProjNoJitter[1][0], Camera.PrevViewProjNoJitter[2][0]));
    float3 previousUp = normalize(float3(Camera.PrevViewProjNoJitter[0][1], Camera.PrevViewProjNoJitter[1][1], Camera.PrevViewProjNoJitter[2][1]));
    float3 world = draw.Position + (right * rotated.x + up * rotated.y) * draw.Size;
    float3 previousWorld = draw.Previous + (previousRight * previousRotated.x + previousUp * previousRotated.y) * draw.PreviousSize;
    VertexOutput result = (VertexOutput)0;
    result.Position = mul(float4(world, 1), Camera.ViewProj);
    result.Current = mul(float4(world, 1), Camera.ViewProjNoJitter);
    result.Previous = mul(float4(previousWorld, 1), Camera.PrevViewProjNoJitter);
    result.UV = corner * float2(1, -1) + 0.5f;
    result.DrawIndex = index;
    result.ViewDepth = draw.ViewDepth;
    return result;
}

struct MeshVertex
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
    float Wing : TEXCOORD0;
};

float3 MeshDirection(float3 local, float wing, float age, float yaw)
{
    float angle = sin(age * 38) * 0.65f * wing;
    float2 flap = float2(local.x * cos(angle) - local.y * sin(angle),
        local.x * sin(angle) + local.y * cos(angle));
    return float3(flap.x * cos(yaw) + local.z * sin(yaw), flap.y,
        local.z * cos(yaw) - flap.x * sin(yaw));
}

VertexOutput MeshVS(MeshVertex vertex, uint instance : SV_InstanceID)
{
    uint index = Indices[Capacity + instance];
    ParticleDraw draw = Draws[index];
    float3 world = draw.Position + MeshDirection(vertex.Position, vertex.Wing, draw.Age, draw.Rotation) * draw.Size;
    float3 previous = draw.Previous + MeshDirection(vertex.Position, vertex.Wing, draw.PreviousAge, draw.PreviousRotation) * draw.PreviousSize;
    VertexOutput result = (VertexOutput)0;
    result.Position = mul(float4(world, 1), Camera.ViewProj);
    result.Current = mul(float4(world, 1), Camera.ViewProjNoJitter);
    result.Previous = mul(float4(previous, 1), Camera.PrevViewProjNoJitter);
    result.DrawIndex = index;
    result.Material = vertex.Color;
    result.Normal = MeshDirection(vertex.Normal, vertex.Wing, draw.Age, draw.Rotation);
    result.ViewDepth = -mul(float4(world, 1), Camera.View).z;
    return result;
}

float4 Shade(VertexOutput input)
{
    ParticleDraw draw = Draws[input.DrawIndex];
    float scene = SceneDepth.Load(int3(uint2(input.Position.xy), 0));
    if (scene > input.Position.z) discard; // RH reverse-Z, sampled depth only; no DSV is bound.
    float fade = 1;
    if (scene > 0 && draw.SoftDistance > 0)
    {
        float distance = Camera.NearPlane * Camera.FarPlane /
            (Camera.NearPlane + scene * (Camera.FarPlane - Camera.NearPlane));
        fade = saturate((distance - input.ViewDepth) / draw.SoftDistance);
    }
    float2 texel = float2(1.0f / draw.Columns, 1.0f / draw.Rows);
    float2 uv = (input.UV + float2(draw.Frame % draw.Columns, draw.Frame / draw.Columns)) * texel;
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float4 material = 0;
    if (draw.Geometry == ParticleGeometryWaterDrop)
    {
        float2 p = (input.UV * 2 - 1) * float2(1,-1);
        float radius = length(p);
        float coverage = 1 - smoothstep(1 - max(fwidth(radius), 0.001f), 1, radius);
        float3 normal = float3(p, sqrt(saturate(1 - dot(p,p))));
        // Water IOR 1.333: Schlick reflectance and a spherical highlight, without emission.
        float fresnel = 0.02037f + 0.97963f * pow(1 - normal.z, 5);
        float highlight = pow(saturate(dot(normal, normalize(float3(-.4f,.65f,.65f)))), 64);
        float3 sky = lerp(float3(.04f,.10f,.14f), float3(.65f,.8f,1), saturate(normal.y * .5f + .5f));
        // shortcut: alpha transmits the scene; add lens refraction if large close-up drops are needed.
        material = float4(sky * (.15f + .85f * fresnel) + highlight,
            coverage * (.15f + .85f * fresnel));
    }
    else if (draw.Geometry == ParticleGeometryFirefly)
    {
        // A fixed key light shows the low-poly facets; the abdomen keeps its emissive radiance.
        float light = 0.25f + 0.75f * saturate(dot(normalize(input.Normal), normalize(float3(0.4f, 0.8f, -0.3f))));
        material = float4(input.Material.rgb * lerp(light, 1, input.Material.a), 1);
    }
    else if (draw.TextureIndex != 0xffffffffu)
    {
        const float2 offsets[4] = { float2(-0.25f, -0.25f), float2(0.25f, 0.25f),
            float2(-0.25f, 0.25f), float2(0.25f, -0.25f) };
        for (uint i = 0; i < draw.EdgeSamples; ++i)
        {
            float2 offset = draw.EdgeSamples == 1 ? 0 : offsets[i];
            float2 sampleUV = clamp(uv + dx * offset.x + dy * offset.y,
                float2(draw.Frame % draw.Columns, draw.Frame / draw.Columns) * texel + texel * 0.001f,
                float2(draw.Frame % draw.Columns + 1, draw.Frame / draw.Columns + 1) * texel - texel * 0.001f);
            material += Textures[NonUniformResourceIndex(draw.TextureIndex)].SampleGrad(LinearClamp, sampleUV, dx, dy);
        }
        material /= draw.EdgeSamples;
    }
    else
    {
        float radius = length(input.UV * 2 - 1);
        float alpha = 1 - smoothstep(1 - max(fwidth(radius), 0.001f), 1, radius);
        material = float4(1, 1, 1, alpha);
    }
    float4 color = material * draw.Color;
    color.a = saturate(color.a * fade);
    if (color.a <= 0.00001f) discard;
    return color;
}

struct WeightedOutput
{
    float4 Accumulation : SV_Target0;
    float Revealage : SV_Target1;
};

WeightedOutput WeightedPS(VertexOutput input)
{
    float4 color = Shade(input);
    float distance = input.ViewDepth;
    float weight = clamp(0.03f / (0.00001f + pow(distance / 200.0f, 4)), 0.01f, 1.0f);
    WeightedOutput result;
    result.Accumulation = float4(color.rgb * color.a, color.a) * weight;
    result.Revealage = color.a;
    return result;
}

float4 ColorPS(VertexOutput input) : SV_Target0
{
    float4 color = Shade(input);
    return float4(color.rgb * color.a, color.a);
}

float4 MotionPS(VertexOutput input) : SV_Target0
{
    float alpha = Shade(input).a;
    float2 motion = ResetMotion != 0 || input.Previous.w <= 0.00001f || input.Current.w <= 0.00001f ? 0 :
        input.Previous.xy / input.Previous.w - input.Current.xy / input.Current.w;
    if (!all(isfinite(motion))) motion = 0;
    // Depth is normalized to the camera far plane to keep half-float accumulation finite.
    return float4(motion, input.ViewDepth / Camera.FarPlane, 1) * alpha;
}

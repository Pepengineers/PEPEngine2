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
    float3 right = float3(Camera.View[0][0], Camera.View[1][0], Camera.View[2][0]);
    float3 up = float3(Camera.View[0][1], Camera.View[1][1], Camera.View[2][1]);
    float3 previousRight = normalize(float3(Camera.PrevViewProjNoJitter[0][0], Camera.PrevViewProjNoJitter[1][0], Camera.PrevViewProjNoJitter[2][0]));
    float3 previousUp = normalize(float3(Camera.PrevViewProjNoJitter[0][1], Camera.PrevViewProjNoJitter[1][1], Camera.PrevViewProjNoJitter[2][1]));
    float3 world = draw.Position + (right * rotated.x + up * rotated.y) * draw.Size;
    float3 previousWorld = draw.Previous + (previousRight * previousRotated.x + previousUp * previousRotated.y) * draw.PreviousSize;
    VertexOutput result;
    result.Position = mul(float4(world, 1), Camera.ViewProj);
    result.Current = mul(float4(world, 1), Camera.ViewProjNoJitter);
    result.Previous = mul(float4(previousWorld, 1), Camera.PrevViewProjNoJitter);
    result.UV = corner * float2(1, -1) + 0.5f;
    result.DrawIndex = index;
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
        fade = saturate((distance - draw.ViewDepth) / draw.SoftDistance);
    }
    float2 texel = float2(1.0f / draw.Columns, 1.0f / draw.Rows);
    float2 uv = (input.UV + float2(draw.Frame % draw.Columns, draw.Frame / draw.Columns)) * texel;
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float4 material = 0;
    if (draw.TextureIndex != 0xffffffffu)
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
    float distance = Draws[input.DrawIndex].ViewDepth;
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
    return float4(motion, Draws[input.DrawIndex].ViewDepth / Camera.FarPlane, 1) * alpha;
}

#include "Particles.hlsli"

StructuredBuffer<Particle> Pool : register(t0);
StructuredBuffer<uint> AliveIn : register(t1);
StructuredBuffer<uint> PoolCounters : register(t2);
RWStructuredBuffer<ParticleDraw> Draws : register(u5);
RWStructuredBuffer<uint> Counts : register(u6); // all, billboard weighted/additive/sorted, mesh weighted/additive
RWStructuredBuffer<uint> Indices : register(u7); // billboard weighted/additive, mesh weighted/additive
RWStructuredBuffer<uint> Keys : register(u8);
RWStructuredBuffer<uint> Payloads : register(u9);
RWStructuredBuffer<uint> DrawArgs : register(u10);
RWStructuredBuffer<uint> SortData : register(u11);

[numthreads(256, 1, 1)]
void Cull(uint id : SV_DispatchThreadID)
{
    if (id >= PoolCounters[1]) return;
    Particle particle = Pool[AliveIn[id]];
    if (Paused != 0)
    {
        particle.Previous = particle.Position;
        particle.PreviousAge = particle.Age;
        particle.PreviousRotation = particle.Rotation;
    }
    float age = saturate(particle.Age / particle.Lifetime);
    float size = lerp(Sizes.x, Sizes.y, age) * particle.SizeRandom * RenderScale;
    float3 view = mul(float4(particle.Position, 1), Camera.View).xyz;
    float4 clip = mul(float4(particle.Position, 1), Camera.ViewProj);
    float radius = size * 0.707107f * (Geometry == ParticleGeometryWaterDrop ? 2.5f : 1);
    float2 margin = radius * float2(length(float3(Camera.ViewProj[0][0], Camera.ViewProj[1][0], Camera.ViewProj[2][0])),
        length(float3(Camera.ViewProj[0][1], Camera.ViewProj[1][1], Camera.ViewProj[2][1])));
    if (-view.z + radius < Camera.NearPlane || -view.z - radius > min(Camera.FarPlane, MaxDistance) ||
        any(abs(clip.xy) > clip.w + margin) || size <= 0 ||
        size * margin.y / max(radius, 0.00001f) * RenderSize.y * 0.5f / max(clip.w, 0.00001f) < MinPixelSize) return;
    float4 color = lerp(ColorStart, ColorEnd, age);
    if (color.a <= 0) return;
    uint drawIndex;
    InterlockedAdd(Counts[0], 1, drawIndex);
    ParticleDraw draw = (ParticleDraw)0;
    draw.Position = particle.Position;
    draw.Previous = particle.Previous;
    draw.Size = size;
    draw.PreviousSize = lerp(Sizes.x, Sizes.y, saturate(particle.PreviousAge / particle.Lifetime)) * particle.SizeRandom * RenderScale;
    draw.Rotation = particle.Rotation;
    draw.PreviousRotation = particle.PreviousRotation;
    draw.Age = particle.Age;
    draw.PreviousAge = particle.PreviousAge;
    draw.Geometry = Geometry;
    draw.Color = color;
    draw.SoftDistance = SoftDistance;
    draw.TextureIndex = TextureIndex;
    uint frames = Columns * Rows;
    draw.Frame = FramesPerSecond == 0 ? min(frames - 1, (uint)(age * frames)) : (uint)(particle.Age * FramesPerSecond) % frames;
    draw.Columns = Columns;
    draw.Rows = Rows;
    draw.EdgeSamples = EdgeSamples;
    draw.ViewDepth = -view.z;
    Draws[drawIndex] = draw;
    uint modeIndex;
    InterlockedAdd(Counts[Geometry == ParticleGeometryFirefly ? BlendMode + 4 : BlendMode + 1], 1, modeIndex);
    if (BlendMode == 2)
    {
        Keys[modeIndex] = ~asuint(max(0.0f, -view.z)); // Positive distance, far to near.
        Payloads[modeIndex] = drawIndex;
    }
    else Indices[(BlendMode + (Geometry == ParticleGeometryFirefly ? 2 : 0)) * TotalCapacity + modeIndex] = drawIndex;
}

[numthreads(1, 1, 1)]
void ClearDraws()
{
    for (uint i = 0; i < 6; ++i) Counts[i] = 0;
}

[numthreads(1, 1, 1)]
void BuildDrawArgs()
{
    for (uint mode = 0; mode < 3; ++mode)
    {
        DrawArgs[mode * 4] = 6;
        DrawArgs[mode * 4 + 1] = Counts[mode + 1];
        DrawArgs[mode * 4 + 2] = 0;
        DrawArgs[mode * 4 + 3] = 0;
    }
    for (uint mode = 0; mode < 2; ++mode)
    {
        DrawArgs[18 + mode * 4] = 120;
        DrawArgs[19 + mode * 4] = Counts[4 + mode];
        DrawArgs[20 + mode * 4] = DrawArgs[21 + mode * 4] = 0;
    }
    uint blocks = (max(Counts[3], 1u) + 511) / 512;
    uint groups = min(blocks, 800u);
    uint reducedPerBin = (groups + 511) / 512;
    SortData[0] = Counts[3];
    SortData[1] = blocks / groups;
    SortData[2] = groups;
    SortData[3] = blocks % groups;
    SortData[4] = reducedPerBin;
    SortData[5] = 16 * reducedPerBin;
    SortData[6] = SortData[7] = 0;
    DrawArgs[12] = groups;
    DrawArgs[13] = DrawArgs[14] = 1;
    DrawArgs[15] = 16 * reducedPerBin;
    DrawArgs[16] = DrawArgs[17] = 1;
}

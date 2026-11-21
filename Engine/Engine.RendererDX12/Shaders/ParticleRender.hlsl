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

void CullParticle(Particle particle)
{
    if (Paused != 0 || ResetMotion != 0)
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
    float3 clipX = float3(Camera.ViewProj[0][0], Camera.ViewProj[1][0], Camera.ViewProj[2][0]);
    float3 clipY = float3(Camera.ViewProj[0][1], Camera.ViewProj[1][1], Camera.ViewProj[2][1]);
    float2 margin = radius * float2(length(clipX), length(clipY));
    bool opaque = BlendMode == ParticleBlendOpaque;
    bool outsideSides = any(abs(clip.xy) > clip.w + margin);
    if (opaque)
    {
        float3 clipW = float3(Camera.ViewProj[0][3], Camera.ViewProj[1][3], Camera.ViewProj[2][3]);
        outsideSides = clip.w + clip.x < -radius * length(clipW + clipX) ||
            clip.w - clip.x < -radius * length(clipW - clipX) ||
            clip.w + clip.y < -radius * length(clipW + clipY) ||
            clip.w - clip.y < -radius * length(clipW - clipY);
    }
    if (-view.z + radius < Camera.NearPlane || -view.z - radius > min(Camera.FarPlane, MaxDistance) ||
        outsideSides || size <= 0 ||
        size * margin.y / max(radius, 0.00001f) * RenderSize.y * 0.5f / max(clip.w, 0.00001f) < MinPixelSize) return;
    float4 color = lerp(ColorStart, ColorEnd, age);
    if (!opaque && color.a <= 0) return;
    // Opaque and transparent draws are prepared separately and reuse the same indirect slots.
    uint blend = opaque ? 0 : BlendMode;
    bool mesh = Geometry == ParticleGeometryFirefly;
    uint countIndex = blend + (mesh ? 4 : 1);
    uint drawIndex, modeIndex;
#if PARTICLE_WAVE_OPS
    uint prefix = WavePrefixCountBits(true);
    uint count = WaveActiveCountBits(true);
    if (WaveIsFirstLane())
    {
        InterlockedAdd(Counts[0], count, drawIndex);
        InterlockedAdd(Counts[countIndex], count, modeIndex);
    }
    drawIndex = WaveReadLaneFirst(drawIndex) + prefix;
    modeIndex = WaveReadLaneFirst(modeIndex) + prefix;
#else
    InterlockedAdd(Counts[0], 1, drawIndex);
    InterlockedAdd(Counts[countIndex], 1, modeIndex);
#endif
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
    if (blend == 2)
    {
        Keys[modeIndex] = ~asuint(max(0.0f, -view.z)); // Positive distance, far to near.
        Payloads[modeIndex] = drawIndex;
    }
    else Indices[(blend + (mesh ? 2 : 0)) * TotalCapacity + modeIndex] = drawIndex;
}

[numthreads(256, 1, 1)]
void Cull(uint id : SV_DispatchThreadID)
{
    if (id < PoolCounters[1]) CullParticle(Pool[AliveIn[id]]);
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

// Dead/alive pool design follows AMD GPUParticles11; explicit reservations replace hidden append counters.
#include "Particles.hlsli"

StructuredBuffer<uint> AliveIn : register(t1);
RWStructuredBuffer<Particle> Pool : register(u0);
RWStructuredBuffer<uint> AliveOut : register(u1);
RWStructuredBuffer<uint> Dead : register(u2);
RWStructuredBuffer<uint> Counters : register(u3);
RWStructuredBuffer<uint> Args : register(u4);
RWStructuredBuffer<ParticleDraw> Draws : register(u5);
RWStructuredBuffer<uint> Counts : register(u6); // all, weighted, additive, sorted
RWStructuredBuffer<uint> Indices : register(u7); // weighted/additive, each TotalCapacity entries
RWStructuredBuffer<uint> Keys : register(u8);
RWStructuredBuffer<uint> Payloads : register(u9);
RWStructuredBuffer<uint> DrawArgs : register(u10);
RWStructuredBuffer<uint> SortData : register(u11);

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float Random(inout uint state)
{
    state = Hash(state);
    return (state >> 8) * (1.0f / 16777216.0f);
}

float3 UnitSphere(inout uint state)
{
    float z = Random(state) * 2 - 1;
    float angle = Random(state) * 6.283185307f;
    return float3(sqrt(max(0, 1 - z * z)) * float2(cos(angle), sin(angle)), z);
}

void DispatchArgs(uint offset, uint count)
{
    Args[offset] = (count + 255) / 256;
    Args[offset + 1] = 1;
    Args[offset + 2] = 1;
}

[numthreads(256, 1, 1)]
void InitializePool(uint id : SV_DispatchThreadID)
{
    if (id < Capacity)
    {
        Pool[id] = (Particle)0;
        Dead[id] = id;
    }
    if (id == 0)
    {
        for (uint i = 0; i < 8; ++i) Counters[i] = 0;
        Counters[0] = Capacity;
    }
}

[numthreads(1, 1, 1)]
void PrepareSimulation()
{
    Counters[2] = 0;
    DispatchArgs(0, Counters[1]);
}

[numthreads(256, 1, 1)]
void Simulate(uint id : SV_DispatchThreadID)
{
    if (id >= Counters[1]) return;
    uint index = AliveIn[id];
    Particle particle = Pool[index];
    particle.Previous = particle.Position;
    particle.PreviousAge = particle.Age;
    particle.PreviousRotation = particle.Rotation;
    particle.Age += DeltaTime;
    if (particle.Age >= particle.Lifetime)
    {
        uint slot;
        InterlockedAdd(Counters[0], 1, slot);
        Dead[slot] = index;
        return;
    }
    float step = DeltaTime / Substeps;
    float damping = exp(-Drag * step);
    for (uint i = 0; i < Substeps; ++i)
    {
        float3 oldVelocity = particle.Velocity;
        particle.Velocity = Wind + (oldVelocity - Wind) * damping +
            Acceleration * (Drag > 0.00001f ? (1 - damping) / Drag : step);
        particle.Position += (oldVelocity + particle.Velocity) * (0.5f * step);
    }
    particle.Rotation += AngularVelocity * DeltaTime;
    if (!all(isfinite(particle.Position)) || !all(isfinite(particle.Velocity)))
    {
        uint slot;
        InterlockedAdd(Counters[0], 1, slot);
        Dead[slot] = index;
        return;
    }
    Pool[index] = particle;
    uint slot;
    InterlockedAdd(Counters[2], 1, slot);
    AliveOut[slot] = index;
}

[numthreads(1, 1, 1)]
void PrepareSpawn()
{
    float requested = EmissionRate * DeltaTime + asfloat(Counters[5]);
    uint continuous = (uint)floor(requested);
    Counters[5] = asuint(requested - continuous);
    uint spawn = Paused != 0 ? 0 : min(Counters[0], min(Capacity, continuous + BurstCount));
    // One bounded reservation, after all deaths: no unsigned decrement can underflow.
    Counters[0] -= spawn;
    Counters[4] = Counters[0];
    Counters[3] = spawn;
    DispatchArgs(3, spawn);
}

[numthreads(256, 1, 1)]
void Spawn(uint id : SV_DispatchThreadID)
{
    if (id >= Counters[3]) return;
    uint index = Dead[Counters[4] + id];
    uint state = Hash(Seed ^ Hash(Counters[6] + id + 1));
    Particle particle = (Particle)0;
    float3 offset = 0;
    if (Shape == 1)
    {
        offset.x = Random(state) * 2 - 1;
        offset.y = Random(state) * 2 - 1;
        offset.z = Random(state) * 2 - 1;
    }
    if (Shape == 2)
    {
        float3 direction = UnitSphere(state);
        float radius = pow(Random(state), 1.0f / 3.0f);
        offset = direction * radius;
    }
    particle.Position = mul(float4(offset * Extents, 1), EmitterWorld).xyz;
    particle.Previous = particle.Position;
    float3 axis = normalize(Direction);
    float3 tangent = normalize(cross(abs(axis.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0), axis));
    float3 bitangent = cross(axis, tangent);
    float cosine = lerp(ConeCos, 1, Random(state));
    float sine = sqrt(max(0, 1 - cosine * cosine));
    float angle = Random(state) * 6.283185307f;
    float3 direction = axis * cosine + (tangent * cos(angle) + bitangent * sin(angle)) * sine;
    direction = normalize(mul(float4(direction, 0), EmitterWorld).xyz);
    particle.Velocity = direction * lerp(Speeds.x, Speeds.y, Random(state));
    particle.Lifetime = lerp(Lifetimes.x, Lifetimes.y, Random(state));
    particle.SizeRandom = 1 + (Random(state) * 2 - 1) * SizeVariation;
    particle.Rotation = particle.PreviousRotation = Random(state) * 6.283185307f;
    particle.Seed = state;
    Pool[index] = particle;
    uint slot;
    InterlockedAdd(Counters[2], 1, slot);
    AliveOut[slot] = index;
}

[numthreads(1, 1, 1)]
void PublishAlive()
{
    Counters[1] = Counters[2];
    Counters[6] += Counters[3];
    DispatchArgs(6, Counters[1]);
}

[numthreads(256, 1, 1)]
void Cull(uint id : SV_DispatchThreadID)
{
    if (id >= Counters[1]) return;
    Particle particle = Pool[AliveIn[id]];
    float age = saturate(particle.Age / particle.Lifetime);
    float size = lerp(Sizes.x, Sizes.y, age) * particle.SizeRandom * RenderScale;
    float3 view = mul(float4(particle.Position, 1), Camera.View).xyz;
    float4 clip = mul(float4(particle.Position, 1), Camera.ViewProj);
    float radius = size * 0.707107f;
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
    InterlockedAdd(Counts[BlendMode + 1], 1, modeIndex);
    if (BlendMode == 2)
    {
        Keys[modeIndex] = ~asuint(max(0.0f, -view.z)); // Positive distance, far to near.
        Payloads[modeIndex] = drawIndex;
    }
    else Indices[BlendMode * TotalCapacity + modeIndex] = drawIndex;
}

[numthreads(1, 1, 1)]
void ClearDraws()
{
    for (uint i = 0; i < 4; ++i) Counts[i] = 0;
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

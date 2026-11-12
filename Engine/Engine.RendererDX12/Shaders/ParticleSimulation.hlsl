// Dead/alive pool design follows AMD GPUParticles11; explicit reservations replace hidden append counters.
#include "Particles.hlsli"
#include "InteractionField.hlsli"
StructuredBuffer<float4> Interaction : register(t0);

StructuredBuffer<uint> AliveIn : register(t1);
StructuredBuffer<Particle> DisplayedPool : register(t2);
Texture2D<float> SceneDepth : register(t3);
RWStructuredBuffer<Particle> Pool : register(u0);
RWStructuredBuffer<uint> AliveOut : register(u1);
RWStructuredBuffer<uint> Dead : register(u2);
RWStructuredBuffer<uint> Counters : register(u3);
RWStructuredBuffer<uint> Args : register(u4);

[numthreads(256, 1, 1)]
void ApplyDisplayHistory(uint id : SV_DispatchThreadID)
{
    if (id >= Capacity) return;
    Particle particle = Pool[id];
    Particle previous = particle;
    if (ResetMotion != 0)
    {
        Particle displayed = DisplayedPool[id];
        if (displayed.Seed == particle.Seed && displayed.Lifetime > 0 && displayed.Age <= particle.Age)
            previous = displayed;
    }
    particle.Previous = previous.Position;
    particle.PreviousAge = previous.Age;
    particle.PreviousRotation = previous.Rotation;
    Pool[id] = particle;
}

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

float3 DepthPosition(int2 pixel, float depth)
{
    float2 ndc = (float2(pixel) + 0.5f) / RenderSize * float2(2, -2) + float2(-1, 1);
    float4 world = mul(float4(ndc, depth, 1), Camera.InvViewProj);
    return world.xyz / world.w;
}

struct ParticleContact
{
    float3 Normal;
    float Time;
    float Push;
};

// shortcut: only frontmost on-screen depth collides; use scene colliders when hidden/off-screen hits are required.
bool FindDepthContact(float3 previous, float3 position, float3 velocity, out ParticleContact contact)
{
    contact = (ParticleContact)0;
    float4 clip = mul(float4(position, 1), Camera.ViewProj);
    if (clip.w <= 0 || clip.z <= 0 || clip.z >= clip.w) return false;
    float2 uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    int2 pixel = int2(floor(uv * RenderSize));
    if (any(pixel < 1) || any(pixel >= int2(RenderSize) - 1)) return false;
    float depth = SceneDepth.Load(int3(pixel, 0));
    if (depth <= 0) return false;
    float3 surface = DepthPosition(pixel, depth);
    float left = SceneDepth.Load(int3(pixel + int2(-1, 0), 0));
    float right = SceneDepth.Load(int3(pixel + int2(1, 0), 0));
    float top = SceneDepth.Load(int3(pixel + int2(0, -1), 0));
    float bottom = SceneDepth.Load(int3(pixel + int2(0, 1), 0));
    // Choose the closest depth neighbour on each axis to avoid normals across silhouettes.
    int2 dx = abs(left - depth) < abs(right - depth) ? int2(-1, 0) : int2(1, 0);
    int2 dy = abs(top - depth) < abs(bottom - depth) ? int2(0, -1) : int2(0, 1);
    float depthX = dx.x < 0 ? left : right;
    float depthY = dy.y < 0 ? top : bottom;
    if (depthX <= 0 || depthY <= 0) return false;
    float3 normal = cross(DepthPosition(pixel + dx, depthX) - surface,
        DepthPosition(pixel + dy, depthY) - surface);
    float lengthSquared = dot(normal, normal);
    if (lengthSquared < 1e-12f) return false;
    normal *= rsqrt(lengthSquared);
    if (dot(normal, Camera.CameraLocation - surface) < 0) normal = -normal;
    float distance = dot(position - surface, normal);
    float previousDistance = dot(previous - surface, normal);
    float incoming = dot(velocity, normal);
    if (incoming < 0 && distance < CollisionRadius && previousDistance >= -CollisionThickness &&
        (previousDistance >= CollisionRadius || distance >= -CollisionThickness))
    {
        contact.Normal = normal;
        contact.Time = saturate((previousDistance - CollisionRadius) / max(previousDistance - distance, 1e-6f));
        contact.Push = max(0, CollisionRadius - lerp(previousDistance, distance, contact.Time)) + 0.01f;
        return true;
    }
    return false;
}

// shortcut: expanded box corners are conservative; use sphere/box edge sweeps if corner clearance matters.
bool FindBoxContact(float3 previous, float3 position, out ParticleContact contact)
{
    contact = (ParticleContact)0;
    float3 origin = mul(float4(previous, 1), WorldToCollisionBox).xyz;
    float3 destination = mul(float4(position, 1), WorldToCollisionBox).xyz;
    float3 travel = destination - origin;
    float3 extent = CollisionBoxHalfExtents + CollisionRadius;
    float entry = 0, exit = 1;
    float3 localNormal = 0;
    if (all(abs(origin) < extent))
    {
        float3 clearance = extent - abs(origin);
        uint axis = clearance.x < clearance.y ? 0 : 1;
        if (clearance.z < clearance[axis]) axis = 2;
        // Avoid dynamic vector writes: FXC otherwise forces the enclosing simulation loops to unroll.
        localNormal = float3(axis == 0, axis == 1, axis == 2) * (origin[axis] < 0 ? -1 : 1);
        contact.Push = clearance[axis] + 0.01f;
    }
    else
    {
        for (uint axis = 0; axis < 3; ++axis)
        {
            if (abs(travel[axis]) < 1e-6f)
            {
                if (abs(origin[axis]) > extent[axis]) return false;
                continue;
            }
            float a = (-extent[axis] - origin[axis]) / travel[axis];
            float b = (extent[axis] - origin[axis]) / travel[axis];
            float nearTime = min(a, b);
            if (nearTime >= entry)
            {
                entry = nearTime;
                localNormal = float3(axis == 0, axis == 1, axis == 2) * (travel[axis] > 0 ? -1 : 1);
            }
            exit = min(exit, max(a, b));
            if (entry > exit) return false;
        }
        if (dot(localNormal, localNormal) == 0) return false;
        contact.Push = 0.01f;
    }
    contact.Normal = mul(localNormal, transpose((float3x3)WorldToCollisionBox));
    contact.Time = entry;
    return true;
}

void ResolveContact(float3 normal, float step, inout float3 velocity)
{
    float incoming = dot(velocity, normal);
    if (incoming >= 0) return;
    // A supporting surface cancels this step's inward gravity instead of generating a fresh bounce.
    float restitution = -incoming <= max(0, -dot(Acceleration, normal)) * step + 0.01f ? 0 : CollisionRestitution;
    float impulse = -(1 + restitution) * incoming;
    float3 tangent = velocity - incoming * normal;
    tangent *= max(0, 1 - CollisionFriction * impulse / max(length(tangent), 1e-6f));
    velocity = tangent - restitution * incoming * normal;
}

// shortcut: resolve at most two contacts per substep; increase the bound for high-speed narrow passages.
void CollideScene(float3 previous, float step, inout float3 position, inout float3 velocity)
{
    float remaining = step;
    for (uint hit = 0; hit < 2; ++hit)
    {
        ParticleContact contact = (ParticleContact)0;
        bool found = false;
        if (DepthCollision != 0) found = FindDepthContact(previous, position, velocity, contact);
        if (CollisionBoxEnabled != 0)
        {
            ParticleContact box;
            if (FindBoxContact(previous, position, box) && (!found || box.Time < contact.Time))
            {
                contact = box;
                found = true;
            }
        }
        if (!found) return;
        position = lerp(previous, position, contact.Time) + contact.Normal * contact.Push;
        ResolveContact(contact.Normal, step, velocity);
        remaining *= 1 - contact.Time;
        if (hit == 1) return;
        previous = position;
        position += velocity * remaining;
    }
}

// shortcut: discard remaining travel after a hit; use swept multi-hit integration for tiny boxes or high speeds.
void ConfineVolume(inout float3 position, inout float3 velocity)
{
    float3 local = mul(float4(position, 1), WorldToEmitter).xyz;
    float3 localVelocity = mul(float4(velocity, 0), WorldToEmitter).xyz;
    for (uint axis = 0; axis < 3; ++axis)
    {
        if (local[axis] < -VolumeHalfExtents[axis])
        {
            local[axis] = -VolumeHalfExtents[axis];
            if (localVelocity[axis] < 0) localVelocity[axis] *= -CollisionRestitution;
        }
        if (local[axis] > VolumeHalfExtents[axis])
        {
            local[axis] = VolumeHalfExtents[axis];
            if (localVelocity[axis] > 0) localVelocity[axis] *= -CollisionRestitution;
        }
    }
    position = mul(float4(local, 1), EmitterWorld).xyz;
    velocity = mul(float4(localVelocity, 0), EmitterWorld).xyz;
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
    if (InteractionResolution != 0 && DeltaTime > 0)
    {
        float4 contact = SampleInteraction(Interaction, particle.Position.xz, InteractionArea, InteractionResolution);
        float heightWeight = saturate((contact.w - particle.Position.y) / 40) *
            saturate((particle.Position.y - InteractionFloor + 20) / 20);
        float presence = contact.z * heightWeight;
        float2 away = contact.xy / max(contact.z, 0.001f);
        away /= max(1, length(away));
        particle.Velocity = lerp(particle.Velocity, Wind + float3(away.x * 180, 220, away.y * 180),
            (1 - exp(-10 * DeltaTime)) * presence);
    }
    float step = DeltaTime / Substeps;
    float damping = exp(-Drag * step);
    for (uint i = 0; i < Substeps; ++i)
    {
        float3 previousPosition = particle.Position;
        float3 oldVelocity = particle.Velocity;
        if (Simulation == 1)
        {
            particle.Velocity += (Acceleration + (Wind - oldVelocity) * Drag) * step;
            particle.Position += particle.Velocity * step;
        }
        else
        {
            particle.Velocity = Wind + (oldVelocity - Wind) * damping +
                Acceleration * (Drag > 0.00001f ? (1 - damping) / Drag : step);
            particle.Position += (oldVelocity + particle.Velocity) * (0.5f * step);
        }
        if ((DepthCollision != 0 || CollisionBoxEnabled != 0) && step > 0)
            CollideScene(previousPosition, step, particle.Position, particle.Velocity);
        if (VolumeEnabled != 0 && step > 0)
            ConfineVolume(particle.Position, particle.Velocity);
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
    float3 localPosition = offset * Extents;
    if (VolumeEnabled != 0) localPosition = clamp(localPosition, -VolumeHalfExtents, VolumeHalfExtents);
    particle.Position = mul(float4(localPosition, 1), EmitterWorld).xyz;
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
    // All reserved spawn lanes survive; append their contiguous range after simulation survivors.
    AliveOut[Counters[2] + id] = index;
}

[numthreads(1, 1, 1)]
void PublishAlive()
{
    Counters[1] = Counters[2] + Counters[3];
    Counters[6] += Counters[3];
    DispatchArgs(6, Counters[1]);
}

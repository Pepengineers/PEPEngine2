// Legacy Muller SPH and MGPU predicted-position signed/near-pressure kernels.
#include "FluidParameters.hlsli"
cbuffer DispatchParameters : register(b0)
{
    uint Count; uint Capacity; uint SortK; uint SortJ;
    float Dt; uint ReferenceUnits; uint2 DispatchPadding;
};
StructuredBuffer<FluidParticle> Particles : register(t0);
StructuredBuffer<float2> Density : register(t1);
StructuredBuffer<uint2> Sorted : register(t2);
StructuredBuffer<uint> Offsets : register(t3);
RWStructuredBuffer<FluidParticle> Next : register(u0);
RWStructuredBuffer<float2> DensityOut : register(u1);
RWStructuredBuffer<uint2> SortOut : register(u2);
RWStructuredBuffer<uint> OffsetOut : register(u3);
static const float Pi = 3.14159265359f;
int3 Cell(float3 position) { return (int3)floor(position / H); }
uint Key(int3 cell)
{
    return ((uint)cell.x * 73856093u ^ (uint)cell.y * 19349663u ^ (uint)cell.z * 83492791u) & (Capacity - 1);
}
float3 Limited(float3 value, float limit)
{
    return value * min(1.0f, limit / max(length(value), 1e-10f));
}
float3 PredictedPosition(FluidParticle particle)
{
    return particle.Position + (particle.Velocity + Gravity * Dt) * PredictionSeconds;
}
[numthreads(128, 1, 1)]
void SavePrevious(uint i : SV_DispatchThreadID)
{
    if (i >= Count) return;
    FluidParticle p = Next[i];
    p.PreviousPosition = p.Position;
    Next[i] = p;
}
[numthreads(128, 1, 1)]
void Hash(uint i : SV_DispatchThreadID)
{
    if (i >= Capacity) return;
    if (i < Count) SortOut[i] = uint2(Key(Cell(PredictedPosition(Particles[i]))), i);
    else SortOut[i] = uint2(0xffffffffu, 0xffffffffu);
    OffsetOut[i] = Count;
}
[numthreads(128, 1, 1)]
void Sort(uint i : SV_DispatchThreadID)
{
    uint other = i ^ SortJ;
    if (i >= Capacity || other <= i || other >= Capacity) return;
    uint2 a = SortOut[i], b = SortOut[other];
    bool greater = a.x > b.x || (a.x == b.x && a.y > b.y);
    if (greater == ((i & SortK) == 0))
    {
        SortOut[i] = b;
        SortOut[other] = a;
    }
}
[numthreads(128, 1, 1)]
void BuildOffsets(uint i : SV_DispatchThreadID)
{
    if (i >= Count) return;
    if (i == 0) OffsetOut[Sorted[i].x] = i;
    else if (Sorted[i - 1].x != Sorted[i].x) OffsetOut[Sorted[i].x] = i;
}
[numthreads(128, 1, 1)]
void Densities(uint i : SV_DispatchThreadID)
{
    if (i >= Count) return;
    float3 position = PredictedPosition(Particles[i]);
    int3 origin = Cell(position);
    float density = 0;
    float nearDensity = 0;
    float h2 = H * H;
    float coefficient = 315.0f / (64.0f * Pi * pow(H, 9));
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        int3 cell = origin + int3(x, y, z);
        uint key = Key(cell);
        for (uint n = Offsets[key]; n < Count; ++n)
        {
            if (Sorted[n].x != key) break;
            FluidParticle neighbor = Particles[Sorted[n].y];
            // Bucket collisions are not cell identity; this also prevents duplicate visits.
            float3 neighborPosition = PredictedPosition(neighbor);
            if (any(Cell(neighborPosition) != cell)) continue;
            float3 d = position - neighborPosition;
            if (NearStiffness > 0)
            {
                float q = max(0, H - length(d));
                density += Mass * 15.0f / (2 * Pi * pow(H, 5)) * q * q;
                nearDensity += Mass * 15.0f / (Pi * pow(H, 6)) * q * q * q;
            }
            else
            {
                float q = max(0, h2 - dot(d, d));
                density += Mass * coefficient * q * q * q;
            }
        }
    }
    DensityOut[i] = max(float2(density, nearDensity), RestDensity * 0.01f);
}
void Collide(inout float3 position, inout float3 velocity)
{
    if (ReferenceUnits != 0)
    {
        // MGPU collides point centers, without a particle-radius margin or outward-velocity test.
        for (uint axis = 0; axis < 3; ++axis)
            if (abs(position[axis]) >= HalfExtent[axis])
            {
                position[axis] = sign(position[axis]) * HalfExtent[axis];
                velocity[axis] *= -Restitution;
            }
        return;
    }
    float3 local = mul(float4(position, 1), WorldToContainer).xyz;
    // World-space sphere support in the local half-spaces, including non-uniform scale.
    float3 margin = Radius * float3(length(WorldToContainer._11_21_31),
        length(WorldToContainer._12_22_32), length(WorldToContainer._13_23_33));
    float3 bound = max(HalfExtent - margin, 0);
    for (uint axis = 0; axis < 3; ++axis)
    {
        float side = local[axis] >= 0 ? 1 : -1;
        if (abs(local[axis]) <= bound[axis]) continue;
        local[axis] = side * bound[axis];
        float3 localNormal = 0;
        localNormal[axis] = side;
        float3 normal = normalize(mul(localNormal, transpose((float3x3)WorldToContainer)));
        float outward = dot(velocity, normal);
        if (outward > 0) velocity -= (1 + Restitution) * outward * normal;
    }
    position = mul(float4(local, 1), ContainerToWorld).xyz;
}
[numthreads(128, 1, 1)]
void Integrate(uint i : SV_DispatchThreadID)
{
    if (i >= Count) return;
    FluidParticle p = Particles[i];
    float rhoi = Density[i].x;
    float pressureI = Stiffness * (NearStiffness > 0 ? rhoi - RestDensity : max(0, rhoi - RestDensity));
    float3 acceleration = Gravity;
    float3 position = PredictedPosition(p);
    int3 origin = Cell(position);
    float gradientCoefficient = -45.0f / (Pi * pow(H, 6));
    uint neighborCount = 0;
    for (int z = -1; z <= 1; ++z)
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        int3 cell = origin + int3(x, y, z);
        uint key = Key(cell);
        for (uint n = Offsets[key]; n < Count; ++n)
        {
            if (Sorted[n].x != key) break;
            uint j = Sorted[n].y;
            if (j == i) continue;
            FluidParticle neighbor = Particles[j];
            float3 neighborPosition = PredictedPosition(neighbor);
            if (any(Cell(neighborPosition) != cell)) continue;
            float3 difference = position - neighborPosition;
            float distance = length(difference);
            if (ReferenceUnits != 0 ? distance > H : distance >= H) continue;
            ++neighborCount;
            // Preserve MGPU's coincident-particle fallback; legacy uses an antisymmetric direction.
            float3 direction = ReferenceUnits != 0 ?
                (distance > 0 ? difference / distance : float3(0,-1,0)) :
                (distance > 1e-6f ? difference / distance : float3(i < j ? -1 : 1, 0, 0));
            float rhoj = Density[j].x;
            float pressureJ = Stiffness * (NearStiffness > 0 ? rhoj - RestDensity : max(0, rhoj - RestDensity));
            float q = H - distance;
            float3 gradient = gradientCoefficient * q * q * direction;
            if (NearStiffness > 0)
            {
                float sharedPressure = (pressureI + pressureJ) * 0.5f;
                float sharedNearPressure = NearStiffness * (Density[i].y + Density[j].y) * 0.5f;
                float densityDerivative = -15.0f / (Pi * pow(H, 5)) * q;
                float nearDerivative = gradientCoefficient * q * q;
                acceleration -= Mass / rhoi * (sharedPressure * densityDerivative / rhoj +
                    sharedNearPressure * nearDerivative / Density[j].y) * direction;
            }
            else
                acceleration -= Mass * (pressureI / (rhoi * rhoi) + pressureJ / (rhoj * rhoj)) * gradient;
            float laplacian = -gradientCoefficient * q;
            acceleration += Viscosity * Mass / rhoj * (neighbor.Velocity - p.Velocity) * laplacian;
        }
    }
    if (ReferenceUnits == 0) acceleration = Limited(acceleration, MaxAcceleration);
    p.Velocity += acceleration * Dt;
    if (ReferenceUnits == 0) p.Velocity = Limited(p.Velocity, MaxSpeed);
    if (NearStiffness > 0 && neighborCount < 8) p.Velocity *= max(0, 1 - 0.75f * Dt);
    p.Position += p.Velocity * Dt;
    Collide(p.Position, p.Velocity);
    if (!all(isfinite(p.Position)) || !all(isfinite(p.Velocity)))
    {
        p.Position = Particles[i].Position;
        p.Velocity = 0;
    }
    Next[i] = p;
}

struct Particle { float3 Position; float3 Velocity; float3 PreviousPosition; };
cbuffer Transfer : register(b0) { uint Count; uint HasNew; };
StructuredBuffer<Particle> Solver : register(t0);
StructuredBuffer<float3> Received : register(t1);
RWStructuredBuffer<float3> Packed : register(u0);
RWStructuredBuffer<Particle> Display : register(u1);
[numthreads(128, 1, 1)]
void Pack(uint i : SV_DispatchThreadID)
{
    if (i < Count) Packed[i] = Solver[i].Position;
}
[numthreads(128, 1, 1)]
void AdvanceDisplay(uint i : SV_DispatchThreadID)
{
    if (i >= Count) return;
    Particle p = Display[i];
    p.PreviousPosition = p.Position;
    if (HasNew != 0) p.Position = Received[i];
    Display[i] = p;
}

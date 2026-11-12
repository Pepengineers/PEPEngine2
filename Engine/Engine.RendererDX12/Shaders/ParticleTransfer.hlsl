#include "Particles.hlsli"

// One fixed-capacity range per emitter: 32-byte header, 32-byte records, 36-byte particles.
cbuffer Transfer : register(b3)
{
    uint Count, ParticleWord, MetadataWord, DisplayBase;
    uint2 EmitterId, Revision;
    uint2 Generation, Sequence;
    uint PayloadWords, EmitterCount, HasNew, ClearHistory;
};
StructuredBuffer<Particle> Source : register(t0);
StructuredBuffer<uint> SourceCounters : register(t1);
StructuredBuffer<uint> Incoming : register(t2);
RWStructuredBuffer<uint> Packed : register(u0);
RWStructuredBuffer<Particle> Display : register(u1);

[numthreads(256, 1, 1)]
void Pack(uint id : SV_DispatchThreadID)
{
    if (id == 0)
    {
        if (MetadataWord == 8)
        {
            Packed[0] = Sequence.x; Packed[1] = Sequence.y;
            Packed[2] = Generation.x; Packed[3] = Generation.y;
            Packed[4] = EmitterCount; Packed[5] = PayloadWords;
            Packed[6] = Packed[7] = 0;
        }
        Packed[MetadataWord] = EmitterId.x; Packed[MetadataWord + 1] = EmitterId.y;
        Packed[MetadataWord + 2] = Revision.x; Packed[MetadataWord + 3] = Revision.y;
        Packed[MetadataWord + 4] = ParticleWord; Packed[MetadataWord + 5] = Count;
        Packed[MetadataWord + 6] = SourceCounters[1]; Packed[MetadataWord + 7] = DisplayBase;
    }
    if (id >= Count) return;
    Particle p = Source[id];
    uint offset = ParticleWord + id * 9;
    Packed[offset] = asuint(p.Position.x); Packed[offset + 1] = asuint(p.Position.y);
    Packed[offset + 2] = asuint(p.Position.z); Packed[offset + 3] = asuint(p.Age);
    Packed[offset + 4] = asuint(p.Lifetime); Packed[offset + 5] = asuint(p.Rotation);
    Packed[offset + 6] = asuint(p.SizeRandom);
    Packed[offset + 7] = p.Identity.x; Packed[offset + 8] = p.Identity.y;
}

[numthreads(256, 1, 1)]
void AdvanceDisplay(uint id : SV_DispatchThreadID)
{
    if (id >= Count) return;
    Particle previous = (Particle)0;
    if ((ClearHistory & 1) == 0) previous = Display[DisplayBase + id];
    Particle p = previous;
    if (HasNew != 0)
    {
        bool valid = all(uint2(Incoming[2], Incoming[3]) == Generation) &&
            all(uint2(Incoming[MetadataWord], Incoming[MetadataWord + 1]) == EmitterId) &&
            all(uint2(Incoming[MetadataWord + 2], Incoming[MetadataWord + 3]) == Revision) &&
            Incoming[MetadataWord + 4] == ParticleWord && Incoming[MetadataWord + 5] == Count;
        p = (Particle)0;
        if (valid)
        {
            uint offset = ParticleWord + id * 9;
            p.Position = asfloat(uint3(Incoming[offset], Incoming[offset + 1], Incoming[offset + 2]));
            p.Age = asfloat(Incoming[offset + 3]); p.Lifetime = asfloat(Incoming[offset + 4]);
            p.Rotation = asfloat(Incoming[offset + 5]); p.SizeRandom = asfloat(Incoming[offset + 6]);
            p.Identity = uint2(Incoming[offset + 7], Incoming[offset + 8]);
        }
    }
    bool same = HasNew != 0 && ClearHistory == 0 && previous.Lifetime > previous.Age &&
        all(p.Identity == previous.Identity);
    p.Previous = same ? previous.Position : p.Position;
    p.PreviousAge = same ? previous.Age : p.Age;
    p.PreviousRotation = same ? previous.Rotation : p.Rotation;
    Display[DisplayBase + id] = p;
}

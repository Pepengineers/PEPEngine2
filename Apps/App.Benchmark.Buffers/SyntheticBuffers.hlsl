static const uint GroupsPerMiB = 32;
static const uint ThreadsPerGroup = 128;
static const uint ElementsPerThread = 16;

cbuffer DispatchParameters : register(b0)
{
    uint Seed;
};

RWStructuredBuffer<uint4> StreamOut : register(u0);
StructuredBuffer<uint4> StreamIn : register(t0);
RWStructuredBuffer<uint> Checksums : register(u0);

groupshared uint PartialChecksums[ThreadsPerGroup];

[numthreads(128, 1, 1)]
void Produce(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    const uint groupIndex = group.y * GroupsPerMiB + group.x;
    const uint start = groupIndex * ThreadsPerGroup * ElementsPerThread + thread;
    [unroll]
    for (uint element = 0; element < ElementsPerThread; ++element)
    {
        const uint index = start + element * ThreadsPerGroup;
        const uint value = (index ^ Seed) * 1664525u + 1013904223u;
        StreamOut[index] = uint4(value, value ^ index, ~value, value * 17u);
    }
}

[numthreads(128, 1, 1)]
void Consume(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    const uint groupIndex = group.y * GroupsPerMiB + group.x;
    const uint start = groupIndex * ThreadsPerGroup * ElementsPerThread + thread;
    uint checksum = Seed + thread;
    [unroll]
    for (uint element = 0; element < ElementsPerThread; ++element)
    {
        const uint4 value = StreamIn[start + element * ThreadsPerGroup];
        checksum += value.x ^ (value.y * 3u) ^ (value.z * 11u) ^ (value.w * 13u);
    }

    PartialChecksums[thread] = checksum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = ThreadsPerGroup / 2; stride > 0; stride /= 2)
    {
        if (thread < stride)
        {
            PartialChecksums[thread] += PartialChecksums[thread + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread == 0)
    {
        Checksums[groupIndex] = PartialChecksums[0];
    }
}

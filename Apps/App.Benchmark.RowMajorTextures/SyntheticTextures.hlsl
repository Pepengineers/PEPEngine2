// Each group processes a 32 by 32 tile. Lanes on each row touch adjacent texels.
static const uint TileSide = 32;
static const uint ThreadRows = 4;
static const uint ThreadsPerGroup = TileSide * ThreadRows;

cbuffer DispatchParameters : register(b0)
{
    uint Extent;
    uint Seed;
};

RWTexture2D<float4> TextureOut : register(u0);
Texture2D<float4> TextureIn : register(t0);
RWStructuredBuffer<uint> Checksums : register(u0);
groupshared uint PartialChecksums[ThreadsPerGroup];

uint PixelValue(uint index, uint seed)
{
    uint value = (index ^ seed) * 1664525u + 1013904223u;
    value ^= value >> 16;
    value *= 2246822519u;
    return value ^ (value >> 13);
}

[numthreads(TileSide, ThreadRows, 1)]
void Produce(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 start = group.xy * TileSide + thread.xy;
    [unroll]
    for (uint row = 0; row < TileSide; row += ThreadRows)
    {
        const uint2 pixel = start + uint2(0, row);
        const uint value = PixelValue(pixel.y * Extent + pixel.x, Seed);
        const uint4 rgba = (value >> uint4(0, 8, 16, 24)) & 0xff;
        TextureOut[pixel] = float4(rgba) / 255.0f;
    }
}

[numthreads(TileSide, ThreadRows, 1)]
void Consume(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const uint2 start = group.xy * TileSide + thread.xy;
    uint sum = 0;
    [unroll]
    for (uint row = 0; row < TileSide; row += ThreadRows)
    {
        const uint2 pixel = start + uint2(0, row);
        const uint4 rgba = uint4(round(TextureIn.Load(int3(pixel, 0)) * 255.0f));
        sum += rgba.x | (rgba.y << 8) | (rgba.z << 16) | (rgba.w << 24);
    }

    PartialChecksums[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = ThreadsPerGroup / 2; stride > 0; stride /= 2)
    {
        if (lane < stride)
        {
            PartialChecksums[lane] += PartialChecksums[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0)
    {
        const uint groupIndex = group.y * (Extent / TileSide) + group.x;
        Checksums[groupIndex] = PartialChecksums[0];
    }
}

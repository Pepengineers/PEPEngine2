// Research candidate only; the production solver still uses its original sort schedule.
#include "../../Shaders/FluidSolver.hlsl"

#ifndef WATER_SORT_TILE_SIZE
#define WATER_SORT_TILE_SIZE 512
#endif

groupshared uint2 TileKeys[WATER_SORT_TILE_SIZE];

void LoadSortTile(uint start, uint lane)
{
    uint a = start + lane;
    uint b = a + WATER_SORT_TILE_SIZE / 2;
    TileKeys[lane] = a < Capacity ? SortOut[a] : uint2(0xffffffffu, 0xffffffffu);
    TileKeys[lane + WATER_SORT_TILE_SIZE / 2] = b < Capacity ? SortOut[b] : uint2(0xffffffffu, 0xffffffffu);
    GroupMemoryBarrierWithGroupSync();
}

void SortTilePairs(uint start, uint lane, uint k, uint j)
{
    // Insert a zero bit at j: each lane owns a disjoint pair, including both writes.
    uint left = (lane & (j - 1)) | ((lane & ~(j - 1)) << 1);
    uint right = left | j;
    uint2 a = TileKeys[left], b = TileKeys[right];
    bool greater = a.x > b.x || (a.x == b.x && a.y > b.y);
    bool swap = greater == (((start + left) & k) == 0);
    TileKeys[left] = swap ? b : a;
    TileKeys[right] = swap ? a : b;
    GroupMemoryBarrierWithGroupSync();
}

void StoreSortTile(uint start, uint lane)
{
    uint a = start + lane;
    uint b = a + WATER_SORT_TILE_SIZE / 2;
    if (a < Capacity) SortOut[a] = TileKeys[lane];
    if (b < Capacity) SortOut[b] = TileKeys[lane + WATER_SORT_TILE_SIZE / 2];
}

[numthreads(WATER_SORT_TILE_SIZE / 2, 1, 1)]
void PresortCS(uint group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint start = group * WATER_SORT_TILE_SIZE;
    LoadSortTile(start, lane);
    for (uint k = 2; k <= min(Capacity, WATER_SORT_TILE_SIZE); k <<= 1)
        for (uint j = k >> 1; j > 0; j >>= 1)
            SortTilePairs(start, lane, k, j);
    StoreSortTile(start, lane);
}

[numthreads(WATER_SORT_TILE_SIZE / 2, 1, 1)]
void MergeTailCS(uint group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint start = group * WATER_SORT_TILE_SIZE;
    LoadSortTile(start, lane);
    for (uint j = WATER_SORT_TILE_SIZE / 2; j > 0; j >>= 1)
        SortTilePairs(start, lane, SortK, j);
    StoreSortTile(start, lane);
}

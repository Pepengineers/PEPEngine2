StructuredBuffer<uint> IDs : register(t0);
RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 5) Output[id.x] = IDs[id.x];
}

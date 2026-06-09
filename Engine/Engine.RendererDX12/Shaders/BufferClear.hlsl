RWStructuredBuffer<uint> CounterBuffer : register(u0);

[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    CounterBuffer[0] = 0;
}
RWStructuredBuffer<uint> CounterBuffer1 : register(u0);
RWStructuredBuffer<uint> CounterBuffer2 : register(u1);


[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    CounterBuffer1[0] = 0;
    CounterBuffer2[0] = 0;
}
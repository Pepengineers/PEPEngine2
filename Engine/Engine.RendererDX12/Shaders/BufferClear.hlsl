#include "CBufferStructures.hlsl"

RWStructuredBuffer<uint> VisibleCommandsCounterBuffer : register(u0);

[numthreads(1, 1, 1)]
void CS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    VisibleCommandsCounterBuffer[0] = 0;
}
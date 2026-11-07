struct Source { float4 PositionRadius; float4 PreviousHeight; };
StructuredBuffer<Source> Sources : register(t0);
RWStructuredBuffer<float4> Trail : register(u0);
RWStructuredBuffer<float4> PreviousTrail : register(u1);
RWStructuredBuffer<float4> Presence : register(u2);
cbuffer Parameters : register(b0)
{
    float4 Area;
    float DeltaSeconds; uint SourceCount, Reset, Resolution;
};
float3 Contact(float2 cellPosition, float2 center, float radius)
{
    float2 offset = cellPosition - center;
    float distance = length(offset);
    float weight = saturate(1 - distance / radius);
    weight = weight * weight * (3 - 2 * weight);
    return float3(offset / max(distance, 0.001f) * weight, weight);
}
[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= Resolution)) return;
    uint cell = id.y * Resolution + id.x;
    float2 cellPosition = Area.xy + ((float2(id) + 0.5f) / Resolution * 2 - 1) * Area.zw;
    float4 current = 0;
    float3 swept = 0;
    // shortcut: O(4096*A) gather; tile sources only if crowd measurements justify it.
    for (uint i = 0; i < SourceCount; ++i)
    {
        Source source = Sources[i];
        float3 contact = Contact(cellPosition, source.PositionRadius.xz, source.PositionRadius.w);
        current.xy += contact.xy;
        current.z = max(current.z, contact.z);
        if (contact.z > 0) current.w = max(current.w, source.PositionRadius.y + source.PreviousHeight.w);
        float2 start = source.PreviousHeight.xz;
        float2 movement = source.PositionRadius.xz - start;
        // A paused frame must not repeatedly stamp the previous frame's swept path.
        if (DeltaSeconds <= 0 || dot(movement, movement) > 500 * 500) start = source.PositionRadius.xz;
        movement = source.PositionRadius.xz - start;
        float t = saturate(dot(cellPosition - start, movement) / max(dot(movement, movement), 0.001f));
        float3 stamp = Contact(cellPosition, start + t * movement, source.PositionRadius.w);
        swept.xy += stamp.xy;
        swept.z = max(swept.z, stamp.z);
    }
    float4 previous = 0;
    if (Reset == 0) previous = Trail[cell];
    float decay = exp(-3 * DeltaSeconds); // 95% recovered after one second, 99.75% after two.
    float4 next = previous * decay;
    if (swept.z >= next.z) next = float4(swept, 0);
    if (DeltaSeconds == 0 && Reset == 0) next = previous;
    PreviousTrail[cell] = Reset != 0 ? next : previous;
    Trail[cell] = next;
    Presence[cell] = current;
}

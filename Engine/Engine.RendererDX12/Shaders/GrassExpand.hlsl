#include "GrassCommon.hlsli"
#include "ShadowFrustum.hlsli"
StructuredBuffer<float3> Roots : register(t0);
StructuredBuffer<float4> Wind : register(t1);
StructuredBuffer<float4> PreviousWind : register(t2);
RWStructuredBuffer<GrassVertex> Vertices : register(u0);
RWStructuredBuffer<uint> Indices : register(u1);
// First 20 bytes are D3D12_DRAW_INDEXED_ARGUMENTS; remaining words are compact counts.
RWStructuredBuffer<uint> Arguments : register(u2);

uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float Random(uint id) { return (Hash(id) & 0x00ffffffu) / 16777216.0f; }
float2 Field(float2 uv, bool previous)
{
    float2 p = clamp(uv * GridResolution - 0.5f, 0, float(GridResolution - 1));
    uint2 low = uint2(floor(p));
    uint2 high = min(low + 1, GridResolution - 1);
    float2 f = frac(p);
    uint4 cells = uint4(low.y * GridResolution + low.x, low.y * GridResolution + high.x,
        high.y * GridResolution + low.x, high.y * GridResolution + high.x);
    float2 a = previous ? PreviousWind[cells.x].xy : Wind[cells.x].xy;
    float2 b = previous ? PreviousWind[cells.y].xy : Wind[cells.y].xy;
    float2 c = previous ? PreviousWind[cells.z].xy : Wind[cells.z].xy;
    float2 d = previous ? PreviousWind[cells.w].xy : Wind[cells.w].xy;
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float3 Bend(float3 root, bool previous)
{
    if (WindMode == 0) return 0;
    float2 uv = (root.xz - WindArea.xy) / (WindArea.zw * 2) + 0.5f;
    if (any(uv < 0) || any(uv > 1)) return 0;
    float2 velocity;
    if (WindMode == 2) velocity = Field(uv, previous);
    else
    {
        float time = previous ? Patch.w : Patch.z;
        float wave = sin(6.2831853f * (time * WindGust.x + dot(root.xz, WindForce.xy) / WindGust.y));
        velocity = WindForce.xy * min(WindGust.z, WindForce.z * (1 + WindForce.w * wave));
    }
    float2 bend = velocity * Lod.z;
    bend *= min(1, Shape.w / max(length(bend), 1e-6f));
    return float3(bend.x, 0, bend.y);
}
[numthreads(1, 1, 1)]
void ClearCS()
{
    for (uint i = 0; i < 8; ++i) Arguments[i] = 0;
    Arguments[1] = 1;
}
[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Count) return;
    uint randomId = Hash(id.x ^ Seed);
    float3 root = PointSource != 0 ? Roots[id.x] :
        float3((Random(randomId) * 2 - 1) * Patch.x, 0, (Random(randomId + 1) * 2 - 1) * Patch.y);
    float height = max(0.01f, Shape.x * (1 + (Random(randomId + 2) * 2 - 1) * Shape.z));
    float3 rootW = mul(float4(root, 1), World).xyz;
    float3 previousRootW = mul(float4(root, 1), PreviousWorld).xyz;
    float3 up = mul(float4(0, height, 0, 0), World).xyz;
    float3 previousUp = mul(float4(0, height, 0, 0), PreviousWorld).xyz;
    float3 extents = abs(up) * 0.5f + (abs(World[0].xyz) + abs(World[2].xyz)) * Shape.y + Shape.w;
    if (!ShadowBoxVisible(rootW + up * 0.5f, extents, ViewProjection)) return;
    float distance = length(rootW - EyeMaxDistance.xyz);
    // Shadow culling has no camera distance cutoff: off-camera casters stay present.
    if (Shadow == 0 && distance > EyeMaxDistance.w) return;
    uint segments = distance > Lod.y && Shadow == 0 ? 1 : (distance > Lod.x ? max(1u, Segments / 2) : Segments);
    bool card = distance > Lod.y && Shadow == 0;
    float previousDistance = length(previousRootW - PreviousEye.xyz);
    bool previousCard = previousDistance > Lod.y && Shadow == 0;
    bool reset = ResetHistory != 0 || card != previousCard;
    uint ribbons = card ? 1 : (Implementation == 1 ? 2 : 1);
    uint vertexCount = (segments + 1) * 2 * ribbons;
    uint indexCount = segments * 6 * ribbons;
    uint vertexBase, indexBase, ignored;
    InterlockedAdd(Arguments[5], vertexCount, vertexBase);
    InterlockedAdd(Arguments[0], indexCount, indexBase);
    InterlockedAdd(Arguments[6], 1, ignored);
    float3 bend = Bend(rootW, false);
    float3 previousBend = Bend(previousRootW, true);
    float angle = Random(randomId + 3) * 6.2831853f;
    for (uint ribbon = 0; ribbon < ribbons; ++ribbon)
    {
        float a = angle + ribbon * 1.5707963f;
        float3 side = mul(float4(cos(a) * Shape.y * 0.5f, 0, sin(a) * Shape.y * 0.5f, 0), World).xyz;
        float3 previousSide = mul(float4(cos(a) * Shape.y * 0.5f, 0, sin(a) * Shape.y * 0.5f, 0), PreviousWorld).xyz;
        if (card)
        {
            float3 toEye = EyeMaxDistance.xyz - rootW;
            float3 toPreviousEye = PreviousEye.xyz - previousRootW;
            float3 sideDirection = cross(up, toEye);
            float3 previousDirection = cross(previousUp, toPreviousEye);
            side = length(side) * (dot(sideDirection, sideDirection) > 1e-8f ? normalize(sideDirection) : float3(1, 0, 0));
            previousSide = length(previousSide) * (dot(previousDirection, previousDirection) > 1e-8f ? normalize(previousDirection) : float3(1, 0, 0));
        }
        uint start = vertexBase + ribbon * (segments + 1) * 2;
        for (uint segment = 0; segment <= segments; ++segment)
        {
            float t = float(segment) / segments;
            float width = card ? 1 : max(0.015f, 1 - t);
            float3 center = rootW + up * t + bend * t * t;
            float3 previousCenter = previousRootW + previousUp * t + previousBend * t * t;
            float3 normal = normalize(cross(side, up + bend * (2 * t)));
            for (uint edge = 0; edge < 2; ++edge)
            {
                GrassVertex vertex;
                vertex.Position = center + side * ((edge * 2.0f - 1) * width);
                vertex.PreviousPosition = reset ? vertex.Position :
                    previousCenter + previousSide * ((edge * 2.0f - 1) * width);
                vertex.Normal = normal;
                vertex.UV = float2(edge, 1 - t);
                Vertices[start + segment * 2 + edge] = vertex;
            }
            if (segment < segments)
            {
                uint offset = indexBase + (ribbon * segments + segment) * 6;
                uint v = start + segment * 2;
                Indices[offset] = v;
                Indices[offset + 1] = v + 2;
                Indices[offset + 2] = v + 1;
                Indices[offset + 3] = v + 1;
                Indices[offset + 4] = v + 2;
                Indices[offset + 5] = v + 3;
            }
        }
    }
}

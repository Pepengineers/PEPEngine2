#include "GrassCommon.hlsli"
#include "InteractionField.hlsli"
StructuredBuffer<float4> Trail : register(t3);
StructuredBuffer<float4> PreviousTrail : register(t4);
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
void ExpandBlade(uint3 id, uint variant)
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
    float3 extents = abs(up) * 0.5f + (abs(World[0].xyz) + abs(World[2].xyz)) * Shape.y + Shape.w + length(up);
#if GRASS_BUILD_ONCE
    uint halfSegments = max(1u, Segments / 2);
    uint fullRibbons = Implementation == 1 ? 2 : 1;
    uint fullVertices = (Segments + 1) * 2 * fullRibbons;
    uint halfVertices = (halfSegments + 1) * 2 * fullRibbons;
    uint segments = variant == 0 ? Segments : variant == 1 ? halfSegments : 1;
    bool card = variant == 2;
    bool previousCard = length(previousRootW - PreviousEye.xyz) > Lod.y;
    bool reset = ResetHistory != 0 || card != previousCard;
    uint ribbons = card ? 1 : fullRibbons;
    uint vertexBase = id.x * (fullVertices + halfVertices + 4) +
        (variant == 0 ? 0 : variant == 1 ? fullVertices : fullVertices + halfVertices);
#else
    if (!ShadowBoxVisible(rootW + up * 0.5f, extents, ViewProjection)) return;
    float distance = length(rootW - EyeMaxDistance.xyz);
    // Shadow culling has no camera distance cutoff: off-camera casters stay present.
    if (Shadow == 0 && distance > EyeMaxDistance.w) return;
    uint segments = distance > Lod.y && Shadow == 0 ? 1 : (distance > Lod.x ? max(1u, Segments / 2) : Segments);
    bool card = distance > Lod.y && Shadow == 0;
    bool previousCard = length(previousRootW - PreviousEye.xyz) > Lod.y;
    bool reset = ResetHistory != 0 || card != previousCard;
    uint ribbons = card ? 1 : (Implementation == 1 ? 2 : 1);
    uint vertexCount = (segments + 1) * 2 * ribbons;
    uint indexCount = segments * 6 * ribbons;
    uint vertexBase, indexBase, ignored;
    InterlockedAdd(Arguments[5], vertexCount, vertexBase);
    InterlockedAdd(Arguments[0], indexCount, indexBase);
    InterlockedAdd(Arguments[6], 1, ignored);
#endif
    float3 bend = Bend(rootW, false);
    float3 previousBend = Bend(previousRootW, true);
    float4 contact = SampleInteraction(Trail, rootW.xz, InteractionArea, Padding0);
    float4 previousContact = SampleInteraction(PreviousTrail, previousRootW.xz, InteractionArea, Padding0);
    float2 direction = dot(contact.xy, contact.xy) > 1e-6f ? normalize(contact.xy) : float2(1, 0);
    float2 previousDirection = dot(previousContact.xy, previousContact.xy) > 1e-6f ? normalize(previousContact.xy) : float2(1, 0);
    bend += float3(direction.x, 0, direction.y) * length(up) * 0.7f * contact.z - up * 0.8f * contact.z;
    previousBend += float3(previousDirection.x, 0, previousDirection.y) * length(previousUp) * 0.7f * previousContact.z -
        previousUp * 0.8f * previousContact.z;
    // Limit the combined wind/contact shape to the stem length, for both motion samples.
    float3 tip = up + bend;
    bend = tip * min(1, length(up) / max(length(tip), 1e-6f)) - up;
    float3 previousTip = previousUp + previousBend;
    previousBend = previousTip * min(1, length(previousUp) / max(length(previousTip), 1e-6f)) - previousUp;
    float angle = Random(randomId + 3) * 6.2831853f;
    for (uint ribbon = 0; ribbon < ribbons; ++ribbon)
    {
        float a = angle + ribbon * 1.5707963f;
        float sine, cosine;
        sincos(a, sine, cosine);
        float4 localSide = float4(cosine * Shape.y * 0.5f, 0, sine * Shape.y * 0.5f, 0);
        float3 side = mul(localSide, World).xyz;
        float3 previousSide = mul(localSide, PreviousWorld).xyz;
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
#if !GRASS_BUILD_ONCE
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
#endif
        }
    }
}

[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID) { ExpandBlade(id, 0); }

#if GRASS_BUILD_ONCE
[numthreads(64, 1, 1)]
void BuildCS(uint3 id : SV_DispatchThreadID)
{
    // Preserve the existing full, reduced and camera-card shapes; views select indices only.
    [loop] for (uint variant = 0; variant < 3; ++variant) ExpandBlade(id, variant);
}

StructuredBuffer<float> GrassHiZ : register(t5);

bool Occluded(float3 center, float3 extents)
{
    if (Padding1 == 0) return false;
    float2 lo = 1, hi = 0;
    float nearest = 0;
    [unroll] for (uint corner = 0; corner < 8; ++corner)
    {
        float3 sign = float3((corner & 1) ? 1 : -1, (corner & 2) ? 1 : -1, (corner & 4) ? 1 : -1);
        float4 clip = mul(float4(center + sign * extents, 1), ViewProjection);
        if (clip.w <= 0 || clip.z < 0 || clip.z >= clip.w) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        lo = min(lo, uv); hi = max(hi, uv); nearest = max(nearest, ndc.z);
    }
    uint2 baseSize = uint2(Padding1, (uint)Lod.w);
    uint2 size = baseSize;
    lo = saturate(lo - 1.0f / size); hi = saturate(hi + 1.0f / size);
    uint level = (uint)ceil(log2(max(1.0f, max((hi.x - lo.x) * size.x, (hi.y - lo.y) * size.y))));
    uint offset = 0, scale = 1;
    for (uint mip = 0; mip < level && any(size > 1); ++mip)
    {
        offset += size.x * size.y;
        size = (size + 1) / 2;
        scale *= 2;
    }
    uint2 a = min(uint2(lo * baseSize / scale), size - 1), b = min(uint2(hi * baseSize / scale), size - 1);
    float furthest = min(min(GrassHiZ[offset + a.y * size.x + a.x], GrassHiZ[offset + a.y * size.x + b.x]),
        min(GrassHiZ[offset + b.y * size.x + a.x], GrassHiZ[offset + b.y * size.x + b.x]));
    // Reverse-Z: only reject bounds entirely behind the furthest occluder.
    return nearest + 0.00001f < furthest;
}

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Count) return;
    uint randomId = Hash(id.x ^ Seed);
    float3 root = PointSource != 0 ? Roots[id.x] :
        float3((Random(randomId) * 2 - 1) * Patch.x, 0, (Random(randomId + 1) * 2 - 1) * Patch.y);
    float height = max(0.01f, Shape.x * (1 + (Random(randomId + 2) * 2 - 1) * Shape.z));
    float3 rootW = mul(float4(root, 1), World).xyz;
    float3 up = mul(float4(0, height, 0, 0), World).xyz;
    float3 extents = abs(up) * 0.5f + (abs(World[0].xyz) + abs(World[2].xyz)) * Shape.y;
    float3 center = rootW + up * 0.5f;
    if (!ShadowBoxVisible(center, extents, ViewProjection)) return;
    float distance = length(rootW - EyeMaxDistance.xyz);
    if (Shadow == 0 && (distance > EyeMaxDistance.w || Occluded(center, extents))) return;
    bool card = distance > Lod.y && Shadow == 0;
    uint halfSegments = max(1u, Segments / 2), fullRibbons = Implementation == 1 ? 2 : 1;
    uint fullVertices = (Segments + 1) * 2 * fullRibbons, halfVertices = (halfSegments + 1) * 2 * fullRibbons;
    uint segments = card ? 1 : distance > Lod.x ? halfSegments : Segments;
    uint ribbons = card ? 1 : fullRibbons;
    uint vertexBase = id.x * (fullVertices + halfVertices + 4) +
        (card ? fullVertices + halfVertices : distance > Lod.x ? fullVertices : 0);
    uint indexCount = segments * 6 * ribbons;
    uint vertexCount = (segments + 1) * 2 * ribbons;
    uint indexBase, ignored;
#if GRASS_WAVE_OPS
    uint prefix = WavePrefixSum(indexCount);
    uint waveIndices = WaveActiveSum(indexCount);
    uint waveVertices = WaveActiveSum(vertexCount);
    uint waveBlades = WaveActiveCountBits(true);
    if (WaveIsFirstLane())
    {
        InterlockedAdd(Arguments[0], waveIndices, indexBase);
        InterlockedAdd(Arguments[5], waveVertices, ignored);
        InterlockedAdd(Arguments[6], waveBlades, ignored);
    }
    indexBase = WaveReadLaneFirst(indexBase) + prefix;
#else
    InterlockedAdd(Arguments[0], indexCount, indexBase);
    InterlockedAdd(Arguments[5], vertexCount, ignored);
    InterlockedAdd(Arguments[6], 1, ignored);
#endif
    for (uint ribbon = 0; ribbon < ribbons; ++ribbon)
        for (uint segment = 0; segment < segments; ++segment)
        {
            uint dst = indexBase + (ribbon * segments + segment) * 6;
            uint v = vertexBase + (ribbon * (segments + 1) + segment) * 2;
            Indices[dst] = v; Indices[dst + 1] = v + 2; Indices[dst + 2] = v + 1;
            Indices[dst + 3] = v + 1; Indices[dst + 4] = v + 2; Indices[dst + 5] = v + 3;
        }
}
#endif

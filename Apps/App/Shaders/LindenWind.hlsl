#include "WindSample.hlsli"

cbuffer WindParameters : register(b0)
{
    float4 WindArea;
    float Time;
    uint Resolution, FragmentCount, VertexCount;
};
struct Fragment { float3 Pivot; float Phase; };
struct FragmentPose { float4 Rotation; float3 Translation; };
StructuredBuffer<Fragment> Fragments : register(t0);

#ifndef LINDEN_DEFORM
StructuredBuffer<float4> Wind : register(t1);
RWStructuredBuffer<FragmentPose> Response : register(u0);

[numthreads(64, 1, 1)]
void EvaluateFragments(uint id : SV_DispatchThreadID)
{
    if (id >= FragmentCount) return;
    Fragment fragment = Fragments[id];
    FragmentPose pose;
    pose.Rotation = float4(0, 0, 0, 1);
    pose.Translation = 0;
    if (Resolution != 0)
    {
        float2 uv = (fragment.Pivot.xz - WindArea.xy) / (2 * WindArea.zw) + 0.5f;
        float2 velocity = SampleWindGrid(Wind, uv, Resolution);
        float speed = length(velocity);
        float3 axis = float3(velocity.y, 0, -velocity.x) / max(speed, 1e-5f);
        // The branch follows the gust; neighboring leaf clusters flutter at independent phases.
        float flutter = sin(Time * 11.3f + fragment.Phase) + 0.4f * sin(Time * 17.9f - fragment.Phase);
        float angle = saturate(speed / 220.0f) * (0.09f + 0.18f * flutter);
        float s, c;
        sincos(0.5f * angle, s, c);
        pose.Rotation = float4(axis * s, c);
        pose.Translation.xz = velocity * 0.035f;
    }
    Response[id] = pose;
}
#else
struct RestVertex { float3 Position; uint FragmentID; float3 Normal; float Weight; float2 UV; };
struct PoseVertex { float3 Position; float3 Normal; };
StructuredBuffer<RestVertex> Rest : register(t1);
StructuredBuffer<FragmentPose> Response : register(t2);
RWStructuredBuffer<PoseVertex> NextPose : register(u0);

float3 RotationOffset(float3 v, float4 q)
{
    return 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

[numthreads(64, 1, 1)]
void DeformVertices(uint id : SV_DispatchThreadID)
{
    if (id >= VertexCount) return;
    RestVertex rest = Rest[id];
    PoseVertex vertex;
    vertex.Position = rest.Position;
    vertex.Normal = rest.Normal;
    if (rest.FragmentID < FragmentCount)
    {
        FragmentPose pose = Response[rest.FragmentID];
        float3 local = rest.Position - Fragments[rest.FragmentID].Pivot;
        float3 delta = pose.Translation + RotationOffset(local, pose.Rotation) * rest.Weight;
        vertex.Position += delta * min(1, 25.0f / max(length(delta), 1e-5f));
        float3 normal = rest.Normal + RotationOffset(rest.Normal, pose.Rotation) * rest.Weight;
        vertex.Normal = normal * rsqrt(max(dot(normal, normal), 1e-12f));
    }
    NextPose[id] = vertex;
}
#endif

#include "CBufferStructures.hlsli"

cbuffer CullingParameters : register(b0)
{
    uint DrawOffset;
    uint ViewOffset;
    uint ResetCache;
};
StructuredBuffer<InstanceData> InstanceCache : register(t0);
StructuredBuffer<IndirectDrawArgs> InputCommands : register(t1);
StructuredBuffer<Material> MaterialCache : register(t2);
StructuredBuffer<ShadowView> ShadowViews : register(t3);
StructuredBuffer<uint> ShadowCacheUpdateFlags : register(t4);
RWStructuredBuffer<ShadowDrawArgs> DirectionalCommands : register(u0);
RWStructuredBuffer<uint> DirectionalCount : register(u1);
RWStructuredBuffer<ShadowDrawArgs> PointSpotStaticCommands : register(u2);
RWStructuredBuffer<uint> PointSpotStaticCount : register(u3);
RWStructuredBuffer<ShadowDrawArgs> PointSpotDynamicCommands : register(u4);
RWStructuredBuffer<uint> PointSpotDynamicCount : register(u5);

[numthreads(1, 1, 1)]
void ClearCounters(uint3 id : SV_DispatchThreadID)
{
    DirectionalCount[0] = 0;
    PointSpotStaticCount[0] = 0;
    PointSpotDynamicCount[0] = 0;
}

[numthreads(64, 1, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    uint drawIndex = DrawOffset + id.x;
    uint count, stride;
    InputCommands.GetDimensions(count, stride);
    if (drawIndex >= count) return;
    IndirectDrawArgs source = InputCommands[drawIndex];
    if (source.InstanceCount == 0) return;
    InstanceData instance = InstanceCache[source.InstanceID];
    if (instance.CastShadows == 0 || MaterialCache[instance.MaterialIndex].RenderLayer == RENDER_LAYER_TRANSPARENT) return;
    uint viewIndex = ViewOffset + id.y;
    ShadowView view = ShadowViews[viewIndex];
    if (view.Type != LIGHT_TYPE_DIRECTIONAL && ResetCache == 0)
    {
        uint update = instance.DynamicShadowCaster != 0 ? SHADOW_UPDATE_FINAL : SHADOW_UPDATE_STATIC;
        if ((ShadowCacheUpdateFlags[view.LightIndex] & update) == 0) return;
    }
    if (!IsBoxVisible(instance.BoundingBoxCenter, instance.BoundingBoxExtents, view.ViewProj)) return;

    ShadowDrawArgs command;
    command.InstanceID = source.InstanceID;
    command.ShadowViewIndex = viewIndex;
    command.IndexCountPerInstance = source.IndexCountPerInstance;
    command.InstanceCount = source.InstanceCount;
    command.StartIndexLocation = source.StartIndexLocation;
    command.BaseVertexLocation = source.BaseVertexLocation;
    command.StartInstanceLocation = source.StartInstanceLocation;
    uint writeIndex;
    if (view.Type == LIGHT_TYPE_DIRECTIONAL)
    {
        InterlockedAdd(DirectionalCount[0], 1, writeIndex);
        DirectionalCommands[writeIndex] = command;
    }
    else if (instance.DynamicShadowCaster != 0)
    {
        InterlockedAdd(PointSpotDynamicCount[0], 1, writeIndex);
        PointSpotDynamicCommands[writeIndex] = command;
    }
    else
    {
        InterlockedAdd(PointSpotStaticCount[0], 1, writeIndex);
        PointSpotStaticCommands[writeIndex] = command;
    }
}

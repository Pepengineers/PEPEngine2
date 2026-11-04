//Values are copied from GDX12DescriptorHeap.h/GDX12SRVHeapIndexAllocation class
#define TEXTURE2D_RANGE_LENGTH 99000
#define TEXTURECUBE_RANGE_LENGTH 5000
#define SHADOWMAP_RANGE_LENGTH 400

struct MainCB
{
    float2 RenderTargetSize;
    float TotalTime;
    float DeltaTime;
    float3 LightDirection;
    float _pad0;
    float3 LightRadiance;
    float _pad1;
    uint LocalLightCount;
    uint SunShadowFlags;
    uint SunShadowRevision;
    uint _pad2;
};

struct Transform
{
    float4x4 World;
    float4x4 PrevWorld;
};

struct Material
{
    float Roughness;
    float Metallic;
    float Opacity;
    uint DiffuseIndex;
    uint NormalIndex;
    uint DisplacementIndex;
    uint RenderLayer; // 0 - opaque, 1 - masked, 2 - transparent
    uint SpecularIndex;
    uint RoughnessIndex;
    uint EmissiveIndex;
    uint Flags; // normal=1, specular=2, roughness=4, emissive=8, baked=16
    uint ReflectionProbePreview; // 0: ordinary material; otherwise one-based published cubemap slot.
    float3 SpecularColor;
    float _pad1;
    float3 EmissiveColor;
    float _pad2;
};

struct CameraCB
{
    float4x4 ViewProj;
    float4x4 ViewProjNoJitter;
    float4x4 View;
    float4x4 InvViewProj;
    float4x4 PrevViewProjNoJitter;
    float3 CameraLocation;
    float NearPlane;
    float FarPlane;
    float _pad1;
    float _pad2;
    float _pad3;
};

struct LightCB
{
    int LightType; //0 - directional; 1 - point; 2 - spot
    float Strength;
    float FalloffStart; // point/spot light only
    float FalloffEnd; // point/spot light only
    float3 Direction; // directional/spot light only
    float SpotLightWidth; // spot light only
    float3 Position; // point/spot light only
    float _pad0;
    float3 Color;
    float _pad1;
    float4x4 World;
    float4x4 ViewProj[6];
    float4x4 ShadowTransform[6];
    float4 CascadeDistances;
};

struct ShadowCB
{
    float4x4 ViewProj[4];
    float4 SplitDepths;
    float4 TexelWorldSize;
};

struct LocalLight
{
    float3 Position;
    float Range;
    float3 Direction;
    float InnerCos;
    float3 Radiance;
    float OuterCos;
    uint Type;
    uint ShadowFirstSlice;
    uint EntityId;
    uint ShadowRevision;
    uint ShadowFlags;
    uint3 _padding;
    float4x4 ShadowViewProj[6];
};

struct InstanceData
{
    uint TransformIndex;
    uint MaterialIndex;
    uint ReflectionDynamic;
    uint _pad2;
    float3 BoundingBoxCenter;
    uint _pad3;
    float3 BoundingBoxExtents;
    uint _pad4;
};

struct IndirectDrawArgs
{
    uint InstanceID;
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int BaseVertexLocation;
    uint StartInstanceLocation;
};

struct IndirectConstants
{
    uint InstanceID;
};

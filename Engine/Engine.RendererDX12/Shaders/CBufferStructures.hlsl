//Values are copied from GDX12DescriptorHeap.h/GDX12SRVHeapIndexAllocation class
#define TEXTURE2D_RANGE_LENGTH 99000
#define TEXTURECUBE_RANGE_LENGTH 5000
#define SHADOWMAP_RANGE_LENGTH 400
#define NO_TEXTURE 0xffffffffu

// Matches MaterialType in GDX12Material.h.
#define RENDER_LAYER_OPAQUE 0u
#define RENDER_LAYER_MASKED 1u
#define RENDER_LAYER_TRANSPARENT 2u

struct MainCB
{
    float2 RenderTargetSize;
    float TotalTime;
    float DeltaTime;
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
    uint DiffuseMapIndex;
    uint NormalMapIndex;
    uint DisplacementMapIndex;
    uint RenderLayer;
    uint UseBakedLighting;
    float3 SpecularColor;
    uint SpecularMapIndex;
    float3 EmissiveColor;
    uint EmissiveMapIndex;
    uint RoughnessMapIndex;
};

struct CameraCB
{
    float4x4 ViewProj;
    float4x4 ViewProjNoJitter;
    float4x4 View;
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

struct InstanceData
{
    uint TransformIndex;
    uint MaterialIndex;
    uint _pad1;
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

float3 SafeNormalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-12f));
}

float3 TransformNormal(float3 normal, float3x3 world)
{
    float3 x = cross(world[1], world[2]);
    float3 y = cross(world[2], world[0]);
    float3 z = cross(world[0], world[1]);
    float signDet = dot(world[0], x) < 0 ? -1.0f : 1.0f;
    return SafeNormalize(mul(normal, float3x3(x, y, z)) * signDet);
}

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

// Matches LightType in GDX12ConstantStructures.h.
#define LIGHT_TYPE_DIRECTIONAL 0u
#define LIGHT_TYPE_POINT 1u
#define LIGHT_TYPE_SPOT 2u

struct GDX12LightConstants
{
    float3 Position;
    float Range;
    float3 Direction;
    uint Type;
    float3 Color;
    float Intensity;
    float InnerConeCos;
    float OuterConeCos;
    uint FirstShadowViewIndex;
};

struct ShadowView
{
    float4x4 ViewProj;
    uint Layer;
    uint Type;
    float SplitDepth;
    float NormalBias;
};

struct LightGridCB
{
    uint TileColumns;
    uint TileRows;
    uint MaskWordsPerCluster;
    uint TileSize;
    uint DepthSlices;
    float DepthSliceScale;
    float DepthSliceBias;
};

struct CameraCB
{
    float4x4 ViewProj;
    float4x4 ViewProjNoJitter;
    float4x4 View;
    float4x4 PrevViewProjNoJitter;
    float4x4 InvViewProj;
    float3 CameraLocation;
    float NearPlane;
    float FarPlane;
    float _pad1;
    float _pad2;
    float _pad3;
};

struct InstanceData
{
    uint TransformIndex;
    uint MaterialIndex;
    uint CastShadows;
    uint DynamicShadowCaster;
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

struct ShadowDrawArgs
{
    uint InstanceID;
    uint ShadowViewIndex;
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int BaseVertexLocation;
    uint StartInstanceLocation;
};

// Tests a world-space AABB against the D3D clip volume, including the 0..w depth range.
bool IsBoxVisible(float3 center, float3 extents, float4x4 m)
{
    float4 planes[6];
    planes[0] = float4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41);
    planes[1] = float4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41);
    planes[2] = float4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42);
    planes[3] = float4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42);
    planes[4] = float4(m._13, m._23, m._33, m._43);
    planes[5] = float4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43);
    for (int i = 0; i < 6; ++i)
    {
        float r = extents.x * abs(planes[i].x) + extents.y * abs(planes[i].y) + extents.z * abs(planes[i].z);
        float d = dot(float4(center, 1.0f), planes[i]);
        if (d + r < 0.0f) return false;
    }
    return true;
}

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

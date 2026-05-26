struct MainCB
{
    float2 RenderTargetSize;
    float TotalTime;
    float DeltaTime;
};

struct TransformCB
{
    float4x4 World;
};

struct MaterialCB
{
    float Roughness;
    float Metallic;
    float _pad1;
    float _pad2;
};

struct CameraCB
{
    float4x4 ViewProj;
    float3 CameraLocation;
    float _pad1;
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
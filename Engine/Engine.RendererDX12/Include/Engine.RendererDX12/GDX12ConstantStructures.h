#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

struct GDX12MainConstants
{
    XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;
    XMFLOAT3 LightDirection = { 0.35f, -1.0f, 0.25f };
    float _pad0 = 0.0f;
    XMFLOAT3 LightRadiance = { 4.0f, 3.8f, 3.5f };
    float _pad1 = 0.0f;
    UINT LocalLightCount = 0;
    UINT SunShadowFlags = 3;
    UINT SunShadowRevision = 1;
    UINT _pad2 = 0;
};

struct GDX12TransformConstants
{
    XMFLOAT4X4 WorldMatrix = Identity4x4();
    XMFLOAT4X4 PrevWorldMatrix = Identity4x4();
};

struct GDX12ShadowConstants
{
    XMFLOAT4X4 ViewProj[4] = { Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4() };
    XMFLOAT4 SplitDepths = {};
    XMFLOAT4 TexelWorldSize = {};
};
static_assert(sizeof(GDX12ShadowConstants) == 288);

struct GDX12MaterialConstants
{
    float Roughness = 0.5f;
    float Metallic = 0.5f;
    float Opacity = 1.0f;
    UINT DiffuseIndex = 0;
    UINT NormalIndex = 0;
    UINT DisplacementIndex = 0;
    UINT RenderLayer = 0; // 0 - opaque, 1 - masked, 2 - transparent
    UINT SpecularIndex = 0;
    UINT RoughnessIndex = 0;
    UINT EmissiveIndex = 0;
    UINT Flags = 0;
    UINT ReflectionProbePreview = 0;
    XMFLOAT3 SpecularColor = { 0.04f, 0.04f, 0.04f };
    float _pad1 = 0.0f;
    XMFLOAT3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
    float _pad2 = 0.0f;
};

static_assert(sizeof(GDX12MainConstants) == 64);
static_assert(sizeof(GDX12MaterialConstants) == 80);

struct GDX12CameraConstants
{
    XMFLOAT4X4 ViewProj = Identity4x4();
    XMFLOAT4X4 ViewProjNoJitter = Identity4x4();
    XMFLOAT4X4 View = Identity4x4();
    XMFLOAT4X4 InvViewProj = Identity4x4();
    XMFLOAT4X4 PrevViewProjNoJitter = Identity4x4();
    XMFLOAT3 CameraLocation = { 0.f, 0.f, 0.f };
    float NearPlane;
    float FarPlane;
    float _pad1;
    float _pad2;
    float _pad3;
};

struct GDX12LightConstants
{
    int LightType = 0; //0 - directional; 1 - point; 2 - spot
    float Strength = 1.f;
    float FalloffStart = 1.0f; // point/spot light only
    float FalloffEnd = 10.0f;  // point/spot light only
    XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f }; // directional/spot light only
    float SpotLightWidth = 64.0f;                        // spot light only
    XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };  // point/spot light only
    float _pad0;
    XMFLOAT3 Color = { 1.f, 1.f, 1.f };
    float _pad1;
    XMFLOAT4X4 World = Identity4x4();
    XMFLOAT4X4 ViewProj[6] = { Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4() };
    XMFLOAT4X4 ShadowTransform[6] = { Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4(), Identity4x4() };
    XMFLOAT4 CascadeDistances = { 10.0f, 50.0f, 150.0f, 400.0f };
};

struct GDX12LocalLightConstants
{
    XMFLOAT3 Position = {};
    float Range = 600.0f;
    XMFLOAT3 Direction = { 0, -1, 0 };
    float InnerCos = 1.0f;
    XMFLOAT3 Radiance = { 1, 1, 1 };
    float OuterCos = 0.5f;
    UINT Type = 1; // 1 point, 2 spot.
    UINT ShadowFirstSlice = UINT_MAX;
    UINT EntityId = 0;
    UINT ShadowRevision = 1;
    UINT ShadowFlags = 0; // Bit 0: casts shadows; bit 1: every-frame updates.
    UINT _padding[3] = {};
    XMFLOAT4X4 ShadowViewProj[6] = {};
};
static_assert(sizeof(GDX12LocalLightConstants) == 464);

struct GDX12InstanceData
{
    UINT TransformIndex;
    UINT MaterialIndex;
    UINT ReflectionDynamic = 0;
    UINT _pad2;
    XMFLOAT3 BoundingBoxCenter;
    UINT _pad3;
    XMFLOAT3 BoundingBoxExtents;
    UINT _pad4;
};

struct GDX12IndirectDrawArgs
{
    UINT InstanceID;
    UINT IndexCountPerInstance;
    UINT InstanceCount;
    UINT StartIndexLocation;
    INT  BaseVertexLocation;
    UINT StartInstanceLocation;
};

struct GDX12IndirectArgs
{
    UINT DrawCount;
    UINT Stride;
    UINT StartOffset;
    UINT Padding;
};

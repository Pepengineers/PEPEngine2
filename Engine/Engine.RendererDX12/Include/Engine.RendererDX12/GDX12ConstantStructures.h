#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

struct GDX12MainConstants
{
    XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;
};

struct GDX12TransformConstants
{
    XMFLOAT4X4 WorldMatrix = Identity4x4();
    XMFLOAT4X4 PrevWorldMatrix = Identity4x4();
};

struct GDX12MaterialConstants
{
    float Roughness = 0.5f;
    float Metallic = 0.5f;
    float Opacity = 1.0f;
    UINT DiffuseMapIndex = 0;
    UINT NormalMapIndex = UINT_MAX;
    UINT DisplacementMapIndex = 0;
    UINT RenderLayer = 0; // 0 - opaque, 1 - masked, 2 - transparent
    UINT UseBakedLighting = 0;
    XMFLOAT3 SpecularColor = {};
    UINT SpecularMapIndex = UINT_MAX;
    XMFLOAT3 EmissiveColor = {};
    UINT EmissiveMapIndex = UINT_MAX;
    UINT RoughnessMapIndex = UINT_MAX;
};
static_assert(sizeof(GDX12MaterialConstants) == 68);

enum class LightType : UINT { Directional = 0, Point = 1, Spot = 2 };

struct GDX12LightConstants
{
    Vector3 Position;
    float Range;
    Vector3 Direction;
    LightType Type;
    Vector3 Color;
    float Intensity;
    float InnerConeCos;
    float OuterConeCos;
    UINT FirstShadowViewIndex = UINT_MAX;
};
static_assert(sizeof(GDX12LightConstants) == 60);

struct ShadowView
{
    XMFLOAT4X4 ViewProj;
    UINT Layer;
    float SplitDepth;
    float NormalBias;
};
static_assert(sizeof(ShadowView) == 76);

struct GDX12LightGridConstants
{
    UINT TileColumns;
    UINT TileRows;
    UINT MaskWordsPerCluster;
    UINT TileSize;
    UINT DepthSlices;
    float DepthSliceScale;
    float DepthSliceBias;
};
static_assert(sizeof(GDX12LightGridConstants) == 7 * sizeof(UINT));

struct GDX12CameraConstants
{
    XMFLOAT4X4 ViewProj = Identity4x4();
    XMFLOAT4X4 ViewProjNoJitter = Identity4x4();
    XMFLOAT4X4 View = Identity4x4();
    XMFLOAT4X4 PrevViewProjNoJitter = Identity4x4();
    XMFLOAT4X4 InvViewProj = Identity4x4();
    XMFLOAT3 CameraLocation = { 0.f, 0.f, 0.f };
    float NearPlane;
    float FarPlane;
    float _pad1;
    float _pad2;
    float _pad3;
};

struct GDX12InstanceData
{
    UINT TransformIndex;
    UINT MaterialIndex;
    UINT _pad1;
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

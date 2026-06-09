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
};

struct GDX12MaterialConstants
{
    float Roughness = 0.5f;
    float Metallic = 0.5f;
    UINT DiffuseIndex;
    UINT NormalIndex;
    UINT DisplacementIndex;
    float _pad1;
    float _pad2;
    float _pad3;
};

struct GDX12CameraConstants
{
    XMFLOAT4X4 ViewProj = Identity4x4();
    XMFLOAT3 CameraLocation = { 0.f, 0.f, 0.f };
    float _pad1;
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